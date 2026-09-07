#include "app.h"
#include "crash_reporter.h"
#include "liblogger/logger.h"

#include <algorithm>
#include <optional>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef RGB
#endif

#include <ftxui/component/event.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/direction.hpp>
#include <ftxui/screen/terminal.hpp>

#include "agent/api/i_backend_admin.h"
#include "agent/api/chat_types.h"
#include "agent/command/inclaude/registry.h"
#include "agent/compact/cache_aware_compactor.h"  // 手动压缩上下文（搜索面板 / /compact）
#include "agent/compact/token_count.h"  // /resume 上下文统计还原
#include "agent/config/app_config.h"
#include "agent/core/chat_session.h"
#include "agent/input/processor.h"
#include "agent/model/context_resolver.h"  // 上下文窗口解析（侧栏进度条）
#include "agent/model/provider_config.h"
#include "agent/model/provider_preset.h"  // find_preset（上下文窗口解析）
#include "agent/mcp/mcp_client_manager.h"  // #27 M4：MCP server 状态查询
#include "agent/session/session_store.h"
#include "agent/skill/inclaude/skill_loader.h"
#include "agent/skill/inclaude/skill_prompt.h"  // build_skills_prompt_section：skills 摘要注入 System Prompt
#include "command/builtins.h"
#include "agent/tool/ShellTool/shell_detector.h"  // ！命令 shell（Windows 优先 Git Bash，降级 cmd.exe）
#include "core/events/stream_events.h"
#include "core/process/subprocess.h"
#include "core/process/tool_registry.h"
#include "core/task/task_manager.h"
#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"
#include "clipboard.h"
#include "render/image_view.h"
#include "render/markdown_to_elements.h"
#include "render/transcript_layout.h"
#include "widgets/sidebar.h"
#include "widgets/sidebar_tabs.h"
#include "widgets/change_viewer.h"
#include "widgets/file_viewer.h"
#include "widgets/project_tree.h"
#include "widgets/status_line.h"
#include "widgets/composer.h"
#include "widgets/suggest_panel.h"
#include "widgets/search_palette.h"
#include "widgets/provider_manager.h"
#include "core/utils/file_index.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

namespace {
/// @brief 输入区内容行数（上限 5 行）
constexpr size_t kComposerMaxLines = 5;
/// @brief 输入区总高度 = 内容行数 + 上下内边距 2
int composer_height(const std::string& input) {
    size_t n = 1;
    for (char c : input)
        if (c == '\n') ++n;
    if (n > kComposerMaxLines) n = kComposerMaxLines;
    return static_cast<int>(n) + 2;
}
/// @brief 侧栏折叠宽度阈值：低于该列宽时不渲染右侧栏（窄屏保护内容区）
constexpr int kSidebarCollapseWidth = 100;

#if defined(_WIN32)

/// @brief 兜底检测真实控制台视口尺寸。
/// FTXUI 的 Terminal::Size() 依赖 GetStdHandle(STD_OUTPUT_HANDLE)，
/// 当 stdout 是管道（如 opencode 终端）时 GetConsoleScreenBufferInfo 失败并回退 80x24，
/// 导致渲染宽度小于实际终端宽度、右侧露出终端原背景。此处改用 CONOUT$ 打开真实控制台。
bool detect_console_size(int& w, int& h) {
    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    bool ok = h_out != nullptr && h_out != INVALID_HANDLE_VALUE &&
              GetConsoleScreenBufferInfo(h_out, &csbi);
    if (!ok) {
        // stdout 是管道（如 opencode 终端）时 GetConsoleScreenBufferInfo 失败，
        // 改用 CONOUT$ 打开真实控制台（ConPTY 下同样可用）。
        h_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (h_out == INVALID_HANDLE_VALUE) return false;
        ok = GetConsoleScreenBufferInfo(h_out, &csbi);
        if (!ok) return false;
    }
    w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return true;
}
#endif

/// @brief 生成 mock 思考内容（思考折叠块演示，随用户消息变化）
std::string build_mock_reasoning(const std::string& user_text) {
    std::string r;
    r += "收到消息：「" + user_text + "」\n";
    r += "1. 解析用户意图，识别关键词与隐含诉求\n";
    r += "2. 若涉及文件/命令操作，规划工具调用顺序\n";
    r += "3. 组织回答结构：结论优先，附代码示例\n";
    r += "4. 检查边界条件：空输入、超长文本、特殊字符\n";
    return r;
}

/// @brief 生成 mock LLM 回复（Markdown 渲染演示）
std::string build_mock_reply(const std::string& user_text) {
    std::string reply;
    reply += "### 收到你的消息\n\n";
    reply += "你说的是：「" + user_text + "」\n\n";
    reply += "**Markdown 语法演示：**\n\n";
    reply += "1. 有序列表\n";
    reply += "2. **加粗**、*斜体*、~~删除线~~\n";
    reply += "3. 行内代码 `std::string`\n\n";
    reply += "---\n\n";
    reply += "```cpp\n";
    reply += "#include <iostream>\n";
    reply += "int main() {\n";
    reply += "    std::cout << \"hello workx\" << std::endl;\n";
    reply += "    return 0;\n";
    reply += "}\n";
    reply += "```\n\n";
    reply += "这是实验 TUI 的 mock 回复，用于验证 Markdown 渲染效果。\n";
    return reply;
}

/// @brief 统计 UTF-8 文本的字符数（去除尾随空白/换行，供「已复制 N 字符」提示）
std::size_t utf8_char_count(std::string_view s) {
    std::size_t end = s.size();
    while (end > 0) {
        const unsigned char c = static_cast<unsigned char>(s[end - 1]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { --end; continue; }
        break;
    }
    s = s.substr(0, end);
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) ++n;  // 非续字节 = 新码点
    }
    return n;
}

/// @brief 截断命令提示语：按显示列上限截断（中文计 2 列），过长以 … 收尾，
///        且截断点回退到 UTF-8 字符边界，避免切断多字节字符。
std::string truncate_palette_text(const std::string& s, std::size_t max_cols = 48) {
    std::size_t cols = 0;
    std::size_t cut = s.size();
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        unsigned width = (c <= 0x7F) ? 1 : 2;  // ASCII 1 列，多字节（中文等）按 2 列
        if (cols + width > max_cols) { cut = i; break; }
        cols += width;
        ++i;
        while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    }
    if (cut == s.size()) return s;
    return s.substr(0, cut) + "…";
}

/// @brief 在文件行中定位修改区块起始行（0-based；找不到返回 -1）
/// @details 以 new_string 首行作锚点，验证后续行连续匹配，避免误命中。
int locate_block(const std::vector<std::string>& lines, const std::string& block) {
    if (block.empty()) return -1;
    std::vector<std::string> blk;
    std::string cur;
    for (const char c : block) {
        if (c == '\n') {
            blk.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty() || block.empty()) blk.push_back(std::move(cur));
    if (blk.empty() || blk.front().empty()) return -1;
    const std::string& first = blk.front();
    for (std::size_t i = 0; i + blk.size() <= lines.size(); ++i) {
        if (lines[i] != first) continue;
        bool match = true;
        for (std::size_t k = 1; k < blk.size(); ++k) {
            if (lines[i + k] != blk[k]) {
                match = false;
                break;
            }
        }
        if (match) return static_cast<int>(i);
    }
    return -1;
}

/// @brief 按行拼接为字符串（每行以 \n 结尾；空列表返回空串）
std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (const auto& l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

/// @brief 截断子 Agent 任务 id（'a'+8 随机十六进制，展示截断到 10 位）
std::string truncate_task_id(const std::string& id) {
    if (id.size() <= 10) return id;
    return id.substr(0, 10);
}

/// @brief 子 Agent 状态 → 中文标签
std::string status_text(const std::string& status) {
    if (status == "running") return std::string(str::kSubStatusRunning);
    if (status == "failed") return std::string(str::kSubStatusFailed);
    return std::string(str::kSubStatusDone);
}

/// @brief 规范化 /view、/edit、/nvim 的文件参数：去首尾空白，剥离前导 '@'
///        （输入 `/view @path` 触发文件搜索面板，选中后保留 '@' 前缀，需去掉再解析路径）
std::string normalize_cmd_path(const std::string& args) {
    std::string s = args;
    const auto is_space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!s.empty() && is_space(s.front())) s.erase(s.begin());
    while (!s.empty() && is_space(s.back())) s.pop_back();
    if (!s.empty() && s.front() == '@') s.erase(s.begin());
    return s;
}
}  // namespace

App::App(AppDeps deps)
    : m_deps(std::move(deps)),
      m_bridge(*m_deps.event_bus, m_queue),
      m_screen(ftxui::ScreenInteractive::Fullscreen()) {
    m_vm.sidebar.model = m_deps.model_name;
    m_vm.sidebar.project = m_deps.project;
    // 上下文窗口（启动时经 resolve_context_length 解析）：侧栏进度条分母
    m_vm.sidebar.context_limit = m_deps.context_limit;

    // B2 统一命令：单一定义 — 内置命令注册进 agent 注册表，副作用回调到 App
    if (!m_deps.command_registry) {
        m_deps.command_registry = std::make_shared<agent::command::CommandRegistry>();
    }
    register_ftx_builtins(*m_deps.command_registry, FtuiCommandCallbacks{
        .on_exit = [this] {
            log_run("ctrl-c exit: on_exit begin");
            m_vm.apply(ActionShutdown{});
            log_run("ctrl-c exit: ActionShutdown applied, pending_exit=" +
                    std::to_string(m_vm.pending_exit));
            if (m_vm.pending_exit) {
                log_run("ctrl-c exit: calling m_screen.Exit()");
                m_screen.Exit();
                log_run("ctrl-c exit: m_screen.Exit() returned");
            }
        },
        .on_model_select = [this] { open_model_selector(); },
        .on_provider_select = [this] { open_provider_palette(); },
        .on_resume = [this](const std::string& args) { cmd_resume(args); },
        .on_rename = [this](const std::string& args) { cmd_rename(args); },
        .on_clear = [this] { cmd_clear(); },
        .on_new = [this] { cmd_new(); },
        .on_compact = [this] { compact_context(); },
        .on_view = [this](const std::string& args) { cmd_view(args); },
        .on_edit = [this](const std::string& args) { cmd_edit(args); },
        .on_nvim = [this](const std::string& args) { cmd_nvim(args); },
        .on_test_askuser = [this] { cmd_test_askuser(); },
    });

    // B2 统一命令：App 持有唯一命令处理器（消费 agent 注册表，含内置命令）
    m_command_processor = std::make_unique<agent::input::InputProcessor>(
        m_deps.command_registry);

    // 加载 skills 并注册为命令 → "/" 提示面板与 Skill 工具共用
    // （对齐 src/app/main.cpp；终端 author 在 ftxtui 路径保留 skill 为斜杠命令可执行）
    // 顺序：先注册 bundled 内置技能（LoadSource::Bundled，具有最高优先级），
    // 再加载磁盘 skills（.claude/skills），同名冲突时磁盘技能不覆盖内置技能。
    {
        namespace fs = std::filesystem;
        const auto bundled_root = agent::skill::find_bundled_skills_dir();
        if (!bundled_root.empty()) {
            agent::skill::register_bundled_skills(*m_deps.command_registry, bundled_root);
        }
        auto base_dirs = agent::skill::find_skill_dirs_up_to_home(fs::current_path().string());
        for (const auto& dir : agent::skill::find_user_skill_dirs()) base_dirs.push_back(dir);
        for (const auto& s : agent::skill::load_skills_from_dirs(base_dirs)) {
            // bundled 优先级最高：磁盘技能若与内置同名则不覆盖
            if (!m_deps.command_registry->exists(s->name())) {
                m_deps.command_registry->register_command(s);
            }
        }
        // conditional skills：会话持有命令注册表（激活匹配 SkillTool 用）
        if (m_deps.session) {
            m_deps.session->set_command_registry(m_deps.command_registry);
            // skills 摘要注入 System Prompt：仅列 name + description + when_to_use，
            // 让模型知晓可用技能而无需加载全文（减少/避免触发 Skill 全文加载导致上下文溢出）。
            const std::string skills_section =
                agent::skill::build_skills_prompt_section(*m_deps.command_registry);
            if (!skills_section.empty()) {
                auto cur = m_deps.session->system_prompt();
                m_deps.session->set_system_prompt(cur + skills_section);
            }
        }
    }

    m_bridge.set_wake_callback([this] { m_screen.PostEvent(Event::Custom); });
    m_bridge.start();

    // #27 M4：查询已连接 MCP server 状态并推送侧栏（连接在 create_session 完成）
    if (m_deps.mcp_manager) {
        std::vector<McpServerLite> servers;
        for (const auto& st : m_deps.mcp_manager->server_status()) {
            servers.push_back(McpServerLite{st.name, st.protocol, st.tool_count,
                                            static_cast<int>(st.state), st.error});
        }
        m_queue.push(ActionMcpStatus{std::move(servers)});
    }

    // 项目文件树（项目 tab）：后台 git 扫描启动（先推 loading 占位，线程完成后推快照）
    start_project_scan();

    // 输入历史：启动时从配置目录加载（~/.workx/history.json）
    m_input_history.load(agent::default_config_path().parent_path() / "history.json");

    // 拖拽选中：FTXUI 原生高亮选区（左键按下/移动/释放由 App::HandleSelection
    // 消费），选区文本变化时缓存，供鼠标释放时写入系统剪贴板。
    m_screen.SelectionChange([this] { on_selection_changed(); });

    // 关闭 FTXUI 对 Ctrl+C 的强制处理：默认 force_handle_ctrl_c_=true 会让
    // 每次 Ctrl+C 都 RecordSignal(SIGINT) → Exit()，导致单次按下即退出。
    // 关闭后仅当组件树未消费该事件时才退出；本应用在 CatchEvent 内自行实现
    // 「1 秒内连按两次 Ctrl+C 才退出」的语义。
    m_screen.ForceHandleCtrlC(false);
}

App::~App() {
    m_anim_run = false;
    m_anim_cv.notify_all();
    if (m_anim_thread.joinable()) m_anim_thread.join();
    m_stream_run = false;
    if (m_stream_thread.joinable()) m_stream_thread.join();
    // ！命令执行线程：join 确保无在途 push 后，队列才随成员析构销毁。
    if (m_cmd_thread.joinable()) m_cmd_thread.join();
    // 复制提示自清除线程不触碰任何成员（析构安全）；join 等待它结束时 m_screen 仍存活。
    if (m_copy_flash_thread.joinable()) m_copy_flash_thread.join();
    // 先置停止信号并 join 项目扫描线程，确保无在途入队后，再停桥接清空队列。
    m_project_watch_run.store(false);
    if (m_project_watch_thread.joinable()) m_project_watch_thread.join();
    // Ctrl+G 异步编辑轮询线程：先停轮询再 join，避免析构期在途读文件/PostEvent
    m_prompt_editing.store(false);
    if (m_prompt_watch_thread.joinable()) m_prompt_watch_thread.join();
#ifdef _WIN32
    if (m_prompt_editor_proc) {
        CloseHandle(static_cast<HANDLE>(m_prompt_editor_proc));
        m_prompt_editor_proc = nullptr;
    }
#endif
    m_bridge.stop();
}

// ---------------------------------------------------------------------------
// 每帧事件处理
// ---------------------------------------------------------------------------

void App::handle_ask_user(const ActionAskUser& a) {
    m_ask_promise = a.result_promise;
    m_ask_cancel = a.cancel_flag;

    // B3：解析全部问题（多问题 + 选项 + 自定义输入开关）
    // 兼容两种 questions 契约：AskUserTool 发布 {questions:[...]} 对象，
    // permission_ask 发布直接数组。
    m_ask_questions.clear();
    m_ask_answers.clear();
    const nlohmann::json* qs = nullptr;
    if (a.questions.is_array()) {
        qs = &a.questions;
    } else if (a.questions.is_object() && a.questions.contains("questions") &&
               a.questions["questions"].is_array()) {
        qs = &a.questions["questions"];
    }
    if (qs) {
        for (const auto& q : *qs) {
            if (!q.is_object()) continue;
            AskQuestion aq;
            aq.question = q.value("question", "");
            aq.header = q.value("header", "");
            aq.multi = q.value("multiSelect", false);
            aq.allow_custom_input = q.value("allow_custom_input", true);
            if (q.contains("options") && q["options"].is_array()) {
                for (const auto& o : q["options"]) {
                    if (o.is_object()) aq.options.push_back(o.value("label", ""));
                    else if (o.is_string()) aq.options.push_back(o.get<std::string>());
                }
            }
            if (aq.question.empty() && !aq.header.empty()) aq.question = aq.header;
            if (!aq.question.empty() && !aq.options.empty()) m_ask_questions.push_back(std::move(aq));
        }
    }

    // 无有效问题：不激活模态，直接以取消结果回填，避免空向量越界崩溃
    if (m_ask_questions.empty()) {
        close_ask(false);
        return;
    }

    m_ask_active = true;
    m_ask_qindex = 0;
    m_ask_sel = 0;
    m_ask_custom = false;
    m_ask_checked.assign(m_ask_questions[0].options.size(), false);
    m_ask_buffer.clear();
    m_ask_deadline = a.timeout_ms > 0
        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(a.timeout_ms)
        : std::chrono::steady_clock::time_point::max();
    if (m_ask_input) m_ask_input->TakeFocus();
}

/// @brief 记录当前问题答案，前进到下一题；最后一题则提交
/// @return true=已全部答完并提交，false=还有下一题
bool App::advance_ask() {
    if (m_ask_qindex >= m_ask_questions.size()) {
        close_ask(true);
        return true;
    }
    const AskQuestion& q = m_ask_questions[m_ask_qindex];
    if (m_ask_custom) {
        // 自定义输入：取文本（去首尾空白）
        std::string ans = m_ask_buffer;
        size_t b = ans.find_first_not_of(" \t");
        size_t e = ans.find_last_not_of(" \t");
        if (b == std::string::npos) ans.clear();
        else ans = ans.substr(b, e - b + 1);
        if (ans.empty()) {
            // 空自定义输入视为取消提交（保留当前题继续）
            return false;
        }
        m_ask_answers.emplace_back(q.question, std::move(ans));
    } else if (q.multi) {
        // 多选：已勾选项用逗号连接
        std::string ans;
        for (size_t i = 0; i < m_ask_checked.size(); ++i) {
            if (m_ask_checked[i] && i < q.options.size()) {
                if (!ans.empty()) ans += ", ";
                ans += q.options[i];
            }
        }
        if (ans.empty()) return false;  // 未选任何项，不前进
        m_ask_answers.emplace_back(q.question, std::move(ans));
    } else {
        // 单选：选中项 label；选到自定义输入选项则进入输入模式
        if (m_ask_sel >= 0 && static_cast<size_t>(m_ask_sel) < q.options.size()) {
            m_ask_answers.emplace_back(q.question, q.options[static_cast<size_t>(m_ask_sel)]);
        } else {
            return false;
        }
    }
    // 下一题
    ++m_ask_qindex;
    if (m_ask_qindex >= m_ask_questions.size()) {
        close_ask(true);
        return true;
    }
    m_ask_sel = 0;
    m_ask_custom = false;
    m_ask_buffer.clear();
    m_ask_checked.assign(m_ask_questions[m_ask_qindex].options.size(), false);
    return false;
}

void App::close_ask(bool submitted) {
    const bool echo = m_ask_test_echo;
    m_ask_test_echo = false;
    if (m_ask_promise) {
        agent::AskUserResult r;
        r.submitted = submitted;
        if (submitted) {
            r.answers = m_ask_answers;
        }
        m_ask_promise->set_value(std::move(r));
        m_ask_promise.reset();
    }
    // /Test:askuser 调试回显：把返回结果作为 assistant 消息展示，便于核对答案
    if (echo) {
        std::string text;
        if (submitted) {
            text = std::string(str::kTestAskUserPrefix);
            for (const auto& [q, a] : m_ask_answers) {
                text += "• " + q + " → " + a + "\n";
            }
        } else {
            text = std::string(str::kTestAskUserCancelled);
        }
        if (!text.empty()) {
            m_vm.apply(ActionAppendMessage{.role = "assistant", .text = std::move(text)});
            m_screen.PostEvent(Event::Custom);
        }
    }
    m_ask_active = false;
    m_ask_buffer.clear();
    m_ask_cancel.reset();
}

void App::drain() {
    // 兜底派发异步事件（EventBus 的 M-8 契约：无事件泵线程时必须显式 drain）
    // AskUserRequestEvent / StreamErrorEvent 等经 publish_async 入队，依赖此处派发
    if (m_deps.event_bus) m_deps.event_bus->process_async_events();

    auto actions = m_queue.drain();
    bool changed = false;
    for (auto& a : actions) {
        try {
            if (auto* ask = std::get_if<ActionAskUser>(&a)) {
                handle_ask_user(*ask);
                changed = true;
                continue;
            }
            if (auto* to = std::get_if<ActionAskUserTimeout>(&a)) {
                if (m_ask_active) close_ask(false);
                changed = true;
                continue;
            }
            if (auto* open_plan = std::get_if<ActionOpenPlan>(&a)) {
                // 退出规划模式：侧边栏预览方案 markdown（复用 /view，同路径仅切 tab）
                cmd_view(open_plan->plan_path);
                changed = true;
                continue;
            }
            if (auto* toast = std::get_if<ActionToast>(&a)) {
                m_vm.prompt_echo = toast->text;
                changed = true;
                continue;
            }
            if (auto* models = std::get_if<ActionModelsLoaded>(&a)) {
                m_model_items = models->models;
                m_model_infos = models->models_info;
                rebuild_model_entries();  // /model 面板条目跟随刷新（active 标记）
                changed = true;
                continue;
            }
            if (auto* sessions = std::get_if<ActionSessionsLoaded>(&a)) {
                m_session_metas = sessions->sessions;
                m_sessions_loading = false;
                // /resume 面板打开中则同步条目（会话可能刚加载完）
                if (m_resume_open) {
                    m_session_entries.clear();
                    for (size_t i = 0; i < m_session_metas.size(); ++i) {
                        const auto& s = m_session_metas[i];
                        m_session_entries.push_back(SearchEntry{
                            .category = SearchCategory::Session,
                            .title = s.title,
                            .subtitle = s.project_name,
                            .keywords = std::to_string(s.message_count)
                                        + std::string(str::kMsgCountKeyword),
                            .payload = static_cast<int>(i),
                        });
                    }
                }
                // 聚合搜索面板（Ctrl+P）打开中：重新装配条目，让会话记录即时出现
                if (m_palette_open) {
                    m_search_entries = assemble_search_entries();
                }
                changed = true;  // 聚合搜索面板下一帧自动重新过滤（数据已更新）
                continue;
            }
            if (auto* switched = std::get_if<ActionProviderSwitched>(&a)) {
                handle_provider_switched(std::move(switched->provider),
                                         switched->model_name, switched->entry);
                changed = true;
                continue;
            }
            if (auto* failed = std::get_if<ActionProviderSwitchFailed>(&a)) {
                handle_provider_switch_failed(failed->provider_name);
                changed = true;
                continue;
            }
            // ！命令 Ctrl+Enter：把结构化命令结果作为用户消息提交给模型
            //（Bash 卡已由 ActionAppendCmdResult 先行渲染，此处仅发送）
            if (auto* cmd2m = std::get_if<ActionSubmitCmdToModel>(&a)) {
                m_vm.apply(ActionAppendMessage{.role = "user", .text = cmd2m->text});
                m_vm.apply(ActionSetBusy{.busy = true});
                m_follow = true;
                if (m_deps.on_submit) {
                    m_deps.on_submit(cmd2m->text, {});
                    // 唤醒事件循环消费积压事件（同 send_input / run_command）
                    m_screen.PostEvent(Event::Custom);
                } else {
                    m_vm.apply(ActionSetBusy{.busy = false});
                }
                changed = true;
                continue;
            }
            changed |= m_vm.apply(a);
        } catch (const std::exception& ex) {
            log_run(std::string("drain: action exception: ") + ex.what());
        }
    }
    // 同步忙标志（供动画线程读取，避免跨线程读 ViewModel）；变化时唤醒动画线程
    const bool new_busy = m_vm.busy;
    if (m_busy.exchange(new_busy) != new_busy) {
        m_anim_cv.notify_all();
    }
    // 消息数镜像（冒烟 driver 只读此 atomic，避免与 UI 线程并发读 ViewModel）
    m_msg_count.store(m_vm.messages.size());
    // B3：AskUser 超时 / cancel_flag 检查（工作线程超时后置位取消标志）
    if (m_ask_active) {
        bool cancelled = m_ask_cancel && m_ask_cancel->load();
        bool expired = m_ask_deadline != std::chrono::steady_clock::time_point::max()
                       && std::chrono::steady_clock::now() >= m_ask_deadline;
        if (cancelled || expired) {
            close_ask(false);
            changed = true;
        }
    }
    if (changed) m_screen.RequestAnimationFrame();
    if (m_vm.pending_exit) m_screen.Exit();
}

void App::log_run(std::string_view msg) {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    // B4：日志路径平台无关，统一写 ~/.workx/logs/workx_tui.log（复用 agent 配置约定）
    namespace fs = std::filesystem;
    fs::path log_path = agent::default_log_path().parent_path() / "workx_tui.log";
    std::error_code ec;
    if (auto parent = log_path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, ec);
    }
    FILE* f = fopen(log_path.string().c_str(), "a");
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[64];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(f, "[%s] %.*s\n", ts, static_cast<int>(msg.size()), msg.data());
    fclose(f);
}

// ---------------------------------------------------------------------------
// 模型选择
// ---------------------------------------------------------------------------

void App::open_model_selector() {
    if (!m_deps.backend_admin) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoBackendModels)});
        return;
    }
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_resume_open = false;
    m_mode_open = false;
    m_provider_open = false;
    m_model_items.clear();
    rebuild_model_entries();  // 空列表（加载中）
    m_model_open = true;
    if (m_model_comp) m_model_comp->TakeFocus();
    // 后台拉取模型列表，完成后入队 action 刷新（避免阻塞 UI）
    std::thread([this]() {
        auto result = m_deps.backend_admin->list_models();
        std::vector<std::string> names;
        std::vector<agent::ModelInfo> infos;
        if (result.is_ok()) {
            infos = result.value();
            for (const auto& m : infos)
                names.push_back(m.name);
        } else {
            names.push_back(std::string(str::kModelListFailed));
        }
        m_queue.push(ActionModelsLoaded{.models = std::move(names),
                                        .models_info = std::move(infos)});
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

void App::rebuild_model_entries() {
    m_model_entries.clear();
    const std::string& cur = m_vm.sidebar.model;  // 当前模型
    for (size_t i = 0; i < m_model_items.size(); ++i) {
        m_model_entries.push_back(SearchEntry{
            .category = SearchCategory::Model,
            .title = m_model_items[i],
            .payload = static_cast<int>(i),
            .active = (m_model_items[i] == cur),
        });
    }
}

void App::apply_model(int index) {
    if (index < 0 || index >= static_cast<int>(m_model_items.size())) return;
    const std::string& name = m_model_items[static_cast<size_t>(index)];
    if (name.rfind(std::string(str::kModelListFailed), 0) == 0) return;  // 占位错误行，忽略
    if (m_deps.backend_admin) m_deps.backend_admin->set_model_name(name);
    m_vm.sidebar.model = name;
    // 持久化模型名：写 backend.model_name 并落盘，否则重启读取旧配置还原为上一模型
    if (m_deps.config_manager) {
        m_deps.config_manager->set(agent::keys::MODEL_NAME, name);
        if (m_deps.save_config) m_deps.save_config();
    }
    // 上下文窗口：模型切换后经 resolver 重解析（provider→cfg→catalog→capability→preset→default）
    if (m_deps.config_manager) {
        int32_t sel_ctx = 0;
        if (index >= 0 && index < static_cast<int>(m_model_infos.size()))
            sel_ctx = m_model_infos[static_cast<size_t>(index)].context_length;
        const std::string provider =
            m_deps.config_manager->get_or<std::string>(agent::keys::PROVIDER, "");
        const agent::ProviderPreset* preset = provider.empty() ? nullptr
                                                               : agent::find_preset(provider);
        auto resolution = agent::resolve_context_length(
            name,
            sel_ctx,
            m_deps.config_manager->get_or<int>(agent::keys::CONTEXT_LENGTH, 0),
            preset,
            m_deps.model_catalog ? m_deps.model_catalog->load() : nullptr);
        m_vm.sidebar.context_limit = resolution.value;
        // 仅当来源是 ProviderList 时持久化（避免用兜底值覆盖用户配置）
        if (resolution.source == agent::ContextLengthResolution::Source::ProviderList) {
            m_deps.config_manager->set(agent::keys::CONTEXT_LENGTH,
                                       static_cast<int>(resolution.value));
        }
    }
    if (m_deps.on_model_changed) m_deps.on_model_changed();
}

// ---------------------------------------------------------------------------
// 模式选择（Ctrl+P → 切换模式：与 /model 同款悬浮选择 + 模式介绍）
// ---------------------------------------------------------------------------

void App::open_mode_selector() {
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_resume_open = false;
    m_model_open = false;
    m_provider_open = false;
    rebuild_mode_entries();  // 3 个模式条目 + active 标记 + 介绍副标题
    m_mode_open = true;
    if (m_mode_comp) m_mode_comp->TakeFocus();
}

void App::rebuild_mode_entries() {
    m_mode_entries.clear();
    const std::string& cur = m_vm.sidebar.mode;  // 当前模式（空=标准）
    struct ModeItem {
        std::string_view label;   ///< "standard" / "plan" / "minimal"
        std::string_view title;   ///< 中文模式名
        std::string_view desc;    ///< 模式介绍（副标题）
    };
    static const ModeItem kModes[] = {
        {"standard", str::kStatusStandard, str::kModeStandardDesc},
        {"minimal", str::kStatusMinimal, str::kModeMinimalDesc},
        {"plan", str::kStatusPlan, str::kModePlanDesc},
    };
    constexpr size_t kModeCount = 3;
    for (size_t i = 0; i < kModeCount; ++i) {
        const bool active = kModes[i].label == cur || (cur.empty() && i == 0);
        m_mode_entries.push_back(SearchEntry{
            .category = SearchCategory::Setting,
            .title = std::string(kModes[i].title),
            .subtitle = std::string(kModes[i].desc),
            .keywords = std::string(kModes[i].label),
            .payload = static_cast<int>(i),
            .active = active,
        });
    }
}

void App::apply_mode(int index) {
    if (index < 0 || index >= 3) return;
    static const char* kLabels[] = {"standard", "minimal", "plan"};
    const std::string label = kLabels[index];
    // 真实模式：session 侧设置（计划联动权限 Plan，退出恢复）并回读；
    // mock 模式：本地直接生效
    if (m_deps.session) {
        switch (index) {
            case 1: m_deps.session->set_session_mode(agent::tool::SessionMode::Minimal); break;
            case 2: m_deps.session->set_session_mode(agent::tool::SessionMode::Plan); break;
            default: m_deps.session->set_session_mode(agent::tool::SessionMode::Standard); break;
        }
        m_vm.sidebar.mode = session_mode_label(m_deps.session->session_mode());
    } else {
        m_vm.sidebar.mode = label;
        m_mock_mode_cycle = index;  // 与循环切换保持同源
    }
    m_vm.apply(ActionSetMode{.label = m_vm.sidebar.mode});
}

void App::cmd_resume(const std::string& args) {
    if (!m_deps.session || m_deps.session_dir.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoSessionBackend)});
        return;
    }

    if (args.empty()) {
        // 无参：打开会话选择面板（复用搜索面板组件）
        open_resume_palette();
        return;
    }

    // 带编号：恢复对应会话
    auto sessions = agent::session::SessionStore::list_sessions(m_deps.session_dir);
    if (sessions.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoHistorySessions)});
        return;
    }
    int idx = 0;
    try { idx = std::stoi(args); } catch (...) { idx = 0; }
    if (idx < 1 || idx > static_cast<int>(sessions.size())) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kResumeBadIndex)});
        return;
    }
    const auto& s = sessions[static_cast<size_t>(idx - 1)];
    const std::string title = s.title.empty() ? s.session_id : s.title;
    resume_session(s.file_path, title);
}

void App::resume_session(const std::string& file_path, const std::string& title) {
    if (!m_deps.session) return;
    if (!m_deps.session->switch_session(file_path)) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kResumeFailedPrefix) + file_path
                    + std::string(str::kCloseParenNl)});
        return;
    }

    // 载入历史消息（含思考/工具卡片；tool 结果按 call_id 回填）
    m_vm.tabs.sub_agents.clear();
    m_vm.tabs.sub_selected = -1;
    m_vm.tabs.changes.changes.clear();
    m_vm.sub_records.clear();  // 切换会话：清空旧子 Agent 记录
    m_vm.sub_active = -1;
    m_vm.output_level = OutputLevel::Main;
    load_session_transcript();

    // #24：同步恢复侧边栏 Todo 清单（与 switch_session 内 restore_todos 同源：
    // load_todos 取 JSONL 最后一条 todo 快照）。避免依赖异步 TodoUpdatedEvent
    // 到达前显示上一会话的清单（事件缺失时 UI 永远错误）。
    m_vm.sidebar.todos = agent::session::SessionStore::load_todos(file_path);

    // 恢复子 Agent 第二层记录（sub_records）：按写入顺序重放持久化事件，
    // 复用 ViewModel 的 apply 逻辑（含 observation 合并到 action 的语义），
    // 同时重建侧边栏任务调度 tab 条目与 Agent 工具卡 sub_task_id 关联。
    for (const auto& ev : agent::session::SessionStore::load_sub_agents(file_path)) {
        if (ev.type == "completed") {
            m_vm.apply(ActionSubAgentCompleted{
                .task_id = ev.task_id,
                .final_answer = ev.final_answer,
                .was_error = ev.was_error,
                .duration_ms = ev.duration_ms,
            });
        } else {
            m_vm.apply(ActionSubAgentProgress{
                .task_id = ev.task_id,
                .step_number = ev.step_number,
                .step_type = ev.step_type,
                .content = ev.content,
                .thought_text = ev.thought_text,
                .tool_name = ev.tool_name,
                .tool_input = ev.tool_input,
                .observation = ev.observation,
                .is_error = ev.is_error,
                .duration_ms = ev.duration_ms,
            });
        }
    }

    // 还原上下文窗口统计：按历史消息估算已用 token，使侧栏进度条立即反映
    // 恢复会话的占用（而非从 0 开始，对齐 src/tui restore_from_history）
    const auto history = m_deps.session->get_messages();
    const int32_t estimated = agent::compact::estimate_messages_tokens(history);
    m_vm.sidebar.context_used = estimated;
    m_vm.sidebar.total_tokens = estimated;
    m_vm.sidebar.cache_read_tokens = 0;
    // #65：历史消息无法还原命中/分项，从 0 起累计（对齐 cache_read_tokens）
    m_vm.sidebar.prompt_tokens = 0;
    m_vm.sidebar.generated_tokens = 0;
    m_vm.sidebar.cache_hit_tokens = 0;
    m_vm.sidebar.cache_miss_tokens = 0;

    m_scroll = 0;
    m_follow = true;
    m_vm.sidebar.title = title;
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kResumedPrefix) + title + std::string(str::kMdBoldEnd)});
}

/// @brief 从当前会话重建转录区（resume 历史载入 / 压缩上下文后刷新共用）
void App::load_session_transcript() {
    m_vm.messages.clear();
    invalidate_msg_cache();
    const auto history = m_deps.session->get_messages();

    // 手动调用技能恢复：把对应 user 消息（技能展开的 query）还原为「原始输入回显 + Skill 卡」。
    // 读取当前会话文件中的 skill 事件，按写入顺序匹配；压缩重建同样覆盖（共享本入口）。
    std::vector<agent::session::SkillEvent> skills;
    const std::string cur_sid = m_deps.session->session_id();
    if (!cur_sid.empty()) {
        const auto skill_path =
            std::filesystem::path(m_deps.session_dir) / (cur_sid + ".jsonl");
        skills = agent::session::SessionStore::load_skills(skill_path.string());
    }
    std::size_t skill_idx = 0;

    int open_idx = -1;  // 正在合并的工具调用 assistant 节点索引（-1=无）
    for (const auto& cm : history) {
        if (cm.role == agent::ChatMessage::Role::User) {
            open_idx = -1;  // 用户消息分隔回合
            // 命中技能事件：该 user 消息是展开的 query → 还原为原始输入回显 + Skill 卡
            if (skill_idx < skills.size() && cm.content == skills[skill_idx].query) {
                const auto& ev = skills[skill_idx];
                ++skill_idx;
                if (!ev.raw_input.empty() && ev.raw_input != cm.content)
                    m_vm.apply(ActionAppendMessage{.role = "user", .text = ev.raw_input});
                m_vm.apply(ActionAppendSkill{.name = ev.name,
                                             .input = ev.input,
                                             .is_error = ev.is_error});
                continue;
            }
            m_vm.apply(ActionAppendMessage{.role = "user", .text = cm.content});
        } else if (cm.role == agent::ChatMessage::Role::Assistant) {
            if (!cm.tool_uses.empty()) {
                // 工具调用消息：合并进当前 open 节点（对齐实时路径：一个回合一条
                // 消息多张卡片相邻，避免恢复会话时卡片间叠加消息级空行成两行间距）
                if (open_idx < 0) {
                    m_vm.messages.push_back(MessageNode{});
                    open_idx = static_cast<int>(m_vm.messages.size()) - 1;
                    auto& open = m_vm.messages[open_idx];
                    open.role = MsgRole::Assistant;
                    open.sealed = true;
                    open.reasoning_expanded = m_vm.card_defaults.reasoning_expanded;
                }
                auto& open = m_vm.messages[open_idx];
                if (!cm.content.empty()) open.text += cm.content;
                if (!cm.reasoning_content.empty()) {
                    open.reasoned = true;
                    if (!open.reasoning.empty()) open.reasoning += "\n";
                    open.reasoning += cm.reasoning_content;
                    open.reasoning_ms += cm.reasoning_ms;
                }
                for (const auto& tu : cm.tool_uses) {
                    ToolCallNode t;
                    t.tool_name = tu.name;
                    t.call_id = tu.id;
                    t.arguments = tu.input.dump();
                    t.text_pos = open.text.size();  // 正文插入点（对齐实时路径）
                    open.tool_calls.push_back(std::move(t));
                }
            } else if (open_idx >= 0) {
                // 同一回合的最终答复：并入 open 节点（对齐实时路径：最终答复与工具卡
                // 同一条消息，避免恢复会话时末卡与最终文本之间叠加消息级空行成两行间距）
                auto& open = m_vm.messages[open_idx];
                if (!cm.content.empty()) {
                    if (!open.text.empty()) open.text += "\n";
                    open.text += cm.content;
                }
                if (!cm.reasoning_content.empty()) {
                    open.reasoned = true;
                    if (!open.reasoning.empty()) open.reasoning += "\n";
                    open.reasoning += cm.reasoning_content;
                    open.reasoning_ms += cm.reasoning_ms;
                }
                open_idx = -1;  // 回合结束
            } else {
                // 独立答复（无工具调用、无 open）：独立节点
                MessageNode n;
                n.role = MsgRole::Assistant;
                n.text = cm.content;
                n.sealed = true;
                if (!cm.reasoning_content.empty()) {
                    n.reasoned = true;
                    n.reasoning = cm.reasoning_content;
                    n.reasoning_expanded = m_vm.card_defaults.reasoning_expanded;
                    n.reasoning_ms = cm.reasoning_ms;
                }
                m_vm.messages.push_back(std::move(n));
            }
        } else if (cm.role == agent::ChatMessage::Role::Tool) {
            // 工具结果：按 call_id 回填对应卡片（出错默认展开，对齐实时路径）
            for (auto it = m_vm.messages.rbegin(); it != m_vm.messages.rend(); ++it) {
                if (auto* t = it->find_tool(cm.tool_call_id)) {
                    t->result = cm.content;
                    t->done = true;
                    t->running = false;
                    t->is_error = cm.is_error;
                    t->expanded = cm.is_error;
                    break;
                }
            }
        }
    }
}

/// @brief 手动压缩上下文（搜索面板「压缩上下文」与 /compact 命令共用）
void App::compact_context() {
    if (!m_deps.session) return;
    // 生成中安全：压缩会就地改写会话消息，先拒绝（不打断正在进行的推理）
    if (m_deps.session->is_generating()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kCompactBusy)});
        m_screen.RequestAnimationFrame();
        return;
    }
    auto result = m_deps.session->compact_context();
    std::string notice;
    switch (result.action) {
        case agent::CacheAwareCompactor::Action::None:
            notice = std::string(str::kCompactNoNeed);
            break;
        case agent::CacheAwareCompactor::Action::SoftNotice:
            notice = std::string(str::kCompactSoft);
            break;
        case agent::CacheAwareCompactor::Action::Stuck:
            notice = std::string(str::kCompactStuck);
            break;
        default:
            // 会话消息已被压缩器就地改写：重建转录区
            load_session_transcript();
            notice = std::string(str::kCompactDonePrefix)
                     + std::to_string(result.tokens_before)
                     + std::string(str::kCompactTokensArrow)
                     + std::to_string(result.tokens_after)
                     + std::string(str::kCompactTokensSuffix);
            break;
    }
    // 刷新侧栏上下文占用统计（还原 / 恢复会话对齐）
    m_vm.sidebar.context_used = result.tokens_after;
    m_vm.sidebar.total_tokens = result.tokens_after;
    m_vm.apply(ActionAppendMessage{.role = "assistant", .text = std::move(notice)});
    m_screen.RequestAnimationFrame();
}

void App::cmd_new() {
    if (!m_deps.session) return;
    m_deps.session->new_session();
    reset_vm_for_new_session();
}

void App::cmd_clear() {
    if (!m_deps.session) return;
    // 记录旧会话文件路径（new_session 会关闭 store 并更换 session_id）
    std::filesystem::path old_file;
    const std::string old_sid = m_deps.session->session_id();
    if (!old_sid.empty())
        old_file = std::filesystem::path(m_deps.session_dir) / (old_sid + ".jsonl");
    // 新建会话（内部：取消任务 + 关闭旧 store + 清空 + 新 session_id）
    m_deps.session->new_session();
    // 删除旧会话文件（store 已关闭，Windows 下可删除）
    if (!old_file.empty()) {
        std::error_code ec;
        std::filesystem::remove(old_file, ec);
        // 从会话缓存剔除已删除文件：m_session_metas 为一次性后台缓存，
        // 不清除会让 /resume 面板与 Ctrl+P 聚合搜索继续列出已删除的会话。
        const std::filesystem::path deleted = old_file;
        for (auto it = m_session_metas.begin(); it != m_session_metas.end(); ) {
            it = (!it->file_path.empty() && std::filesystem::path(it->file_path) == deleted)
                     ? m_session_metas.erase(it)
                     : ++it;
        }
    }
    reset_vm_for_new_session();
}

void App::reset_vm_for_new_session() {
    m_vm.messages.clear();
    invalidate_msg_cache();
    m_vm.tabs.sub_agents.clear();
    m_vm.tabs.sub_selected = -1;
    m_vm.sub_records.clear();
    m_vm.sub_active = -1;
    m_vm.output_level = OutputLevel::Main;
    m_vm.tabs.changes.changes.clear();
    // 标题栏回退「新会话」；统计从零开始（新会话上下文）
    m_vm.sidebar.title.clear();
    // #24：新会话清空 Todo 清单（与 ChatSession::new_session 的 restore_todos
    // 空快照同源；同步清空避免异步 TodoUpdatedEvent 到达前残留旧会话清单）
    m_vm.sidebar.todos.clear();
    m_vm.sidebar.context_used = 0;
    m_vm.sidebar.total_tokens = 0;
    m_vm.sidebar.prompt_tokens = 0;
    m_vm.sidebar.generated_tokens = 0;
    m_vm.sidebar.cache_read_tokens = 0;
    m_vm.sidebar.cache_hit_tokens = 0;
    m_vm.sidebar.cache_miss_tokens = 0;
    m_scroll = 0;
    m_follow = true;
}

void App::cmd_rename(const std::string& args) {
    if (args.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kRenameUsage)});
        return;
    }
    auto store = m_deps.session ? m_deps.session->session_store() : nullptr;
    if (store) store->append_title(args);
    m_vm.sidebar.title = args;
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kRenamedPrefix) + args + std::string(str::kMdBoldEnd)});
}

/// @brief /view：打开文件只读查看器（读取 ≤2MB + 按行切分 + 语言推断 + 定位）
/// @details 相对路径基于当前工作目录解析；超限截断并提示。打开后切到文件 tab。
void App::cmd_view(const std::string& args) {
    // 去首尾空白 + 剥离前导 '@'（@path 面板触发语义）
    const std::string path = normalize_cmd_path(args);
    if (path.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kViewUsage)});
        return;
    }

    namespace fs = std::filesystem;
    fs::path p(path);
    if (!p.is_absolute()) p = fs::current_path() / p;

    // 已打开同一文件：仅切到文件 tab（保留滚动位置），不重读
    if (m_vm.tabs.file_open && m_vm.tabs.file.path == p.string()) {
        m_vm.tabs.active = SidebarTab::kFiles;
        m_screen.RequestAnimationFrame();
        return;
    }

    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kViewNotFound) + path + std::string(str::kCloseParenNl)});
        return;
    }

    // —— 图片文件：/view 与项目树点击共用此路径（stb 解码 → 半块 truecolor 预览）——
    if (is_image_file(p.string())) {
        std::string err;
        auto img = decode_image_file(p.string(), &err);
        m_vm.tabs.file.path = p.string();
        m_vm.tabs.file.lang.clear();
        m_vm.tabs.file.scroll = 0;
        m_vm.tabs.file.dirty = false;
        m_vm.tabs.file.changes.clear();
        m_vm.tabs.file.image = std::move(img);
        if (m_vm.tabs.file.image) {
            m_vm.tabs.file.lines.clear();
        } else {
            m_vm.tabs.file.lines = {"（图片解码失败：" + err + "）"};
        }
        m_vm.tabs.file_open = true;
        m_vm.tabs.active = SidebarTab::kFiles;
        m_screen.RequestAnimationFrame();
        return;
    }

    // 读取（≤2MB，超限截断并提示）
    constexpr std::uintmax_t kMaxViewSize = 2u * 1024u * 1024u;
    const std::uintmax_t size = fs::file_size(p, ec);
    const bool truncated = !ec && size > kMaxViewSize;

    std::ifstream in(p, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (truncated && content.size() > kMaxViewSize)
        content.resize(static_cast<std::size_t>(kMaxViewSize));

    // 按行切分（兼容 \r\n / \n）
    std::vector<std::string> lines;
    std::string cur;
    for (const char c : content) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty() || content.empty()) lines.push_back(std::move(cur));

    // 收集该文件会话内修改（内联 diff 高亮），并定位修改区块起始行
    const std::string abs_path = fs::weakly_canonical(p, ec).string();
    m_vm.tabs.file.changes.clear();
    for (const auto& ch : m_vm.tabs.changes.changes) {
        fs::path cp(ch.file_path);
        if (!cp.is_absolute()) cp = fs::current_path() / cp;
        std::error_code ec2;
        if (fs::weakly_canonical(cp, ec2).string() != abs_path) continue;
        FileChange copy = ch;
        const int start = locate_block(lines, ch.new_string);
        if (start >= 0) copy.new_start = start + 1;  // 1-based
        m_vm.tabs.file.changes.push_back(std::move(copy));
    }

    m_vm.tabs.file.path = p.string();
    m_vm.tabs.file.image.reset();
    m_vm.tabs.file.lines = std::move(lines);
    m_vm.tabs.file.lang = lang_from_path(p.string());
    m_vm.tabs.file.scroll = 0;
    m_vm.tabs.file.dirty = false;
    m_vm.tabs.file_open = true;
    m_vm.tabs.active = SidebarTab::kFiles;
    if (truncated)
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kViewTooLarge)});
    m_screen.RequestAnimationFrame();
}

/// @brief /edit：内嵌 nvim 编辑文件（方案 B）
/// @details 流程：解析路径 → 校验文件 → 检测 nvim → 暂停模型 → 打开文件 tab →
///          WithRestoredIO 全屏切换启动 nvim（模态）→ 返回后重读文件。
///          闭包必须在 UI 线程执行（Uninstall/Install 直接操作终端句柄）。
void App::cmd_edit(const std::string& args) {
    // 去首尾空白 + 剥离前导 '@'（@path 面板触发语义）
    const std::string path = normalize_cmd_path(args);
    if (path.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditUsage)});
        return;
    }

    namespace fs = std::filesystem;
    fs::path p(path);
    if (!p.is_absolute()) p = fs::current_path() / p;

    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditIsDir) + path + std::string(str::kCloseParenNl)});
        return;
    }
    // 允许不存在的路径：/edit 支持新建空文件（nvim 保存后自动重读）
    const bool is_new = !fs::is_regular_file(p, ec);

    // 检测 nvim（PATH 查找；缺失则提示并中止，不进入编辑）
    auto nvim = agent::process::ToolRegistry::instance().find_executable("nvim");
    if (!nvim) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditNoNvim)});
        return;
    }

    // 暂停模型活动，避免后台写文件与手动编辑冲突
    if (m_deps.session) m_deps.session->cancel_current_task();

    if (is_new) {
        // 新建文件：初始化空文件 tab（无磁盘内容可读）
        m_vm.tabs.file.path = p.string();
        m_vm.tabs.file.lines.clear();
        m_vm.tabs.file.image.reset();
        m_vm.tabs.file.changes.clear();
        m_vm.tabs.file.lang = lang_from_path(p.string());
        m_vm.tabs.file.scroll = 0;
        m_vm.tabs.file.dirty = false;
        m_vm.tabs.file_open = true;
        m_vm.tabs.active = SidebarTab::kFiles;
    } else {
        // 打开文件 tab 显示当前内容（复用 /view 读取 + 行号 + 内联 diff）
        cmd_view(path);
    }

    if (is_new)
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditNewFile)});

    // WithRestoredIO：临时卸载 FTXUI 终端钩子 → 全屏 nvim（模态）→ 恢复 TUI
    const std::string abs_path = fs::weakly_canonical(p, ec).string();
    bool launched = false;
    int exit_code = 0;
    auto edit = m_screen.WithRestoredIO([&] {
        auto r = agent::process::exec_interactive(*nvim, {abs_path});
        if (r.is_ok()) {
            launched = true;
            exit_code = r.value().exit_code;
        }
    });
    edit();

    if (!launched) {
        // 启动失败：不重读文件，避免把未修改内容误报为"编辑完成"
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditFailed)});
        m_screen.RequestAnimationFrame();
        return;
    }

    // 编辑返回：重读文件（与磁盘一致），刷新文件 tab
    reload_file();
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = exit_code == 0 ? std::string(str::kEditSaved)
                               : std::string(str::kEditAborted)});
    m_screen.RequestAnimationFrame();
}

/// @brief /nvim：启动 nvim（可带文件路径；WithRestoredIO 全屏切换，模态）
void App::cmd_nvim(const std::string& args) {
    // 去首尾空白 + 剥离前导 '@'（@path 面板触发语义）；空 = 在当前目录启动
    const std::string path = normalize_cmd_path(args);

    auto nvim = agent::process::ToolRegistry::instance().find_executable("nvim");
    if (!nvim) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditNoNvim)});
        return;
    }
    // 暂停模型活动，避免后台写文件与手动编辑冲突
    if (m_deps.session) m_deps.session->cancel_current_task();

    bool launched = false;
    auto edit = m_screen.WithRestoredIO([&] {
        std::vector<std::string> argv;
        if (!path.empty()) {
            namespace fs = std::filesystem;
            fs::path p(path);
            if (!p.is_absolute()) p = fs::current_path() / p;
            argv.push_back(p.string());
        }
        auto r = agent::process::exec_interactive(*nvim, argv);
        if (r.is_ok()) launched = true;
    });
    edit();

    if (!launched) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kEditFailed)});
    }
    m_screen.RequestAnimationFrame();
}

/// @brief Ctrl+G：打开系统默认编辑器编辑当前输入（Prompt 文件双向同步）
void App::edit_prompt() {
    namespace fs = std::filesystem;
    // 1. Prompt 文件路径（与输入历史同目录约定：~/.workx/prompt.md）
    const fs::path prompt = agent::default_config_path().parent_path() / "prompt.md";
    std::error_code ec;
    fs::create_directories(prompt.parent_path(), ec);

    // 2. 把当前输入框内容同步到 Prompt 文件
    {
        std::ofstream out(prompt, std::ios::binary | std::ios::trunc);
        if (out) out.write(m_input_buffer.data(),
                           static_cast<std::streamsize>(m_input_buffer.size()));
    }

    // 3. 解析默认编辑器（$WORKX_EDITOR → $EDITOR；否则 Windows=记事本，POSIX=nvim/vim/nano）
    std::string editor_cmd;
    std::vector<std::string> editor_args;
    // 环境变量可能形如 "code --wait"，拆分首 token 为命令名、余下为前置参数
    auto split_first = [](const std::string& s, std::string& cmd,
                          std::vector<std::string>& args) {
        const auto sp = s.find(' ');
        if (sp == std::string::npos) { cmd = s; return; }
        cmd = s.substr(0, sp);
        std::string rest = s.substr(sp + 1);
        size_t b = 0;
        while (b < rest.size()) {
            while (b < rest.size() && rest[b] == ' ') ++b;
            if (b >= rest.size()) break;
            const size_t en = rest.find(' ', b);
            args.push_back(rest.substr(b, en == std::string::npos ? rest.size() : en - b));
            b = en == std::string::npos ? rest.size() : en;
        }
    };
    const char* env = std::getenv("WORKX_EDITOR");
    if (!env || !*env) env = std::getenv("EDITOR");
    if (env && *env) {
        split_first(env, editor_cmd, editor_args);
    } else {
#ifdef _WIN32
        editor_cmd = "notepad.exe";
#else
        auto pick = [&](const char* name) {
            if (!editor_cmd.empty()) return;
            if (auto p = agent::process::ToolRegistry::instance().find_executable(name))
                editor_cmd = *p;
        };
        pick("nvim");
        pick("vim");
        pick("nano");
#endif
    }

    if (editor_cmd.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string("Ctrl+G：未找到可用编辑器，请设置 $EDITOR 或安装 nvim/vim/nano 之一")});
        m_screen.RequestAnimationFrame();
        return;
    }

    // 4a. Windows 记事本：走异步分支（TUI 与 GUI 记事本窗口并存，后台轮询实时同步，
    //     按 Esc 收尾）。规避 Win11 Store 版 notepad stub 进程提前退出、
    //     无法以「进程退出」作为编辑完成信号的问题。
    m_prompt_path = prompt.string();
#ifdef _WIN32
    {
        std::string low = editor_cmd;
        for (char& c : low)
            c = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
        if (low == "notepad" || low == "notepad.exe") {
            start_prompt_editor_async();
            return;
        }
    }
#endif

    // 4b. 终端型编辑器（POSIX 或 Windows 自定义 TUI 编辑器）：阻塞式全屏编辑
    const std::string path_str = prompt.string();
    bool launched = false;
    auto edit = m_screen.WithRestoredIO([&] {
        std::vector<std::string> args = editor_args;
        args.push_back(path_str);
        auto r = agent::process::exec_interactive(editor_cmd, args);
        if (r.is_ok()) launched = true;
    });
    edit();

    if (!launched) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string("Ctrl+G：编辑器启动失败，输入内容未更改")});
        m_screen.RequestAnimationFrame();
        return;
    }

    // 5. 编辑器退出后读回 Prompt 文件（与磁盘一致）
    m_input_buffer = load_prompt_file(path_str);
    m_composer_cursor = m_input_buffer.size();
    if (m_composer) m_composer->TakeFocus();
    m_screen.RequestAnimationFrame();
}

/// @brief 读 Prompt 文件并归一化（剥 UTF-8 BOM、CRLF/孤立 CR→LF、去尾换行）
std::string App::load_prompt_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (content.size() >= 3 && static_cast<unsigned char>(content[0]) == 0xEF &&
        static_cast<unsigned char>(content[1]) == 0xBB &&
        static_cast<unsigned char>(content[2]) == 0xBF)
        content.erase(0, 3);
    std::string lf;
    lf.reserve(content.size());
    for (size_t i = 0; i < content.size(); ++i) {
        const char ch = content[i];
        if (ch == '\r') {
            if (i + 1 < content.size() && content[i + 1] == '\n') continue;  // CRLF → LF
            lf.push_back('\n');  // 孤立 CR → LF
        } else {
            lf.push_back(ch);
        }
    }
    while (!lf.empty() && lf.back() == '\n') lf.pop_back();
    return lf;
}

/// @brief Windows notepad 异步分支：后台线程轮询 Prompt 文件实时同步输入框，Esc 结束
void App::start_prompt_editor_async() {
    if (m_prompt_editing.load()) return;  // 防重入：编辑会话已在进行
    if (m_prompt_path.empty()) return;

    // 记录本轮基线，清空残留 pending
    {
        std::lock_guard<std::mutex> lock(m_prompt_mutex);
        m_prompt_last.clear();
        m_prompt_pending.clear();
        m_prompt_pending_dirty = false;
    }
    m_prompt_editing.store(true);

#ifdef _WIN32
    {
        // UTF-8 路径 → 宽字符（notepad 命令行用）
        auto to_wide = [](const std::string& s) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
            std::wstring w(static_cast<size_t>(n > 0 ? n : 1), L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
            return w;
        };
        std::wstring wpath = to_wide(m_prompt_path);
        std::wstring cmdline = L"notepad.exe \"" + wpath + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                            CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi)) {
            m_prompt_editing.store(false);
            m_vm.apply(ActionAppendMessage{.role = "assistant",
                .text = std::string("Ctrl+G：记事本启动失败，输入内容未更改")});
            m_screen.RequestAnimationFrame();
            return;
        }
        CloseHandle(pi.hThread);
        m_prompt_editor_proc = pi.hProcess;
    }
#endif

    // 后台轮询线程：
    //  1) 检测到 Prompt 文件内容变化（保存）→ 投递 Custom，UI 同步输入框（保存即同步）；
    //  2) 自动检测记事本关闭：首轮判 stub（启动即退出的 Win11 Store 版 stub 按进程退出
    //     检测永远为真、无法当作「关窗」信号），真实长命记事本在首轮后退出 = 关闭窗口，
    //     置 m_prompt_auto_done 由 UI 线程自动收尾读回（关闭时自动保存）。
    m_prompt_watch_thread = std::thread([this] {
        bool first_check = true;   // 首次是否已做 stub 判定
        bool stub = false;         // 启动早期即退出的 stub（禁用自动关闭检测）
        while (m_prompt_editing.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            // —— 同步：文件变化（保存）→ 投递 UI ——
            std::string lf = load_prompt_file(m_prompt_path);
            {
                std::lock_guard<std::mutex> lock(m_prompt_mutex);
                if (lf != m_prompt_last) {
                    m_prompt_last = lf;
                    m_prompt_pending = lf;
                    m_prompt_pending_dirty = true;
                    m_screen.PostEvent(ftxui::Event::Custom);
                }
            }
            // —— 自动关闭检测（Windows）——
#ifdef _WIN32
            const HANDLE proc = static_cast<HANDLE>(m_prompt_editor_proc);
            if (proc) {
                const DWORD st = WaitForSingleObject(proc, 0);
                if (first_check) {
                    first_check = false;
                    if (st == WAIT_OBJECT_0) stub = true;  // 启动即已退出 → stub，禁用自动检测
                }
                if (!stub && st == WAIT_OBJECT_0) {  // 真实记事本进程退出 = 关闭窗口
                    m_prompt_auto_done.store(true);
                    m_screen.PostEvent(ftxui::Event::Custom);
                    break;
                }
            }
#endif
        }
    });

    if (m_composer) m_composer->TakeFocus();
    m_screen.RequestAnimationFrame();
}

/// @brief 结束异步编辑会话（Esc）：停轮询线程 + 最后读回 Prompt 文件同步输入框
void App::finish_prompt_editor() {
    if (!m_prompt_editing.load()) return;
    m_prompt_editing.store(false);
    if (m_prompt_watch_thread.joinable()) m_prompt_watch_thread.join();
#ifdef _WIN32
    if (m_prompt_editor_proc) {
        CloseHandle(static_cast<HANDLE>(m_prompt_editor_proc));
        m_prompt_editor_proc = nullptr;
    }
#endif
    if (!m_prompt_path.empty())
        m_input_buffer = load_prompt_file(m_prompt_path);
    m_composer_cursor = m_input_buffer.size();
    if (m_composer) m_composer->TakeFocus();
    m_screen.RequestAnimationFrame();
}

/// @brief UI 线程消费轮询线程的最新内容（Custom 事件）
void App::drain_prompt_pending() {
    std::string content;
    {
        std::lock_guard<std::mutex> lock(m_prompt_mutex);
        if (!m_prompt_pending_dirty) return;
        m_prompt_pending_dirty = false;
        content = std::move(m_prompt_pending);
    }
    m_input_buffer = std::move(content);
    m_composer_cursor = m_input_buffer.size();
    m_screen.RequestAnimationFrame();
}

/// @brief 重读当前文件 tab 内容（/edit 返回后与磁盘保持一致）
void App::reload_file() {
    if (!m_vm.tabs.file_open || m_vm.tabs.file.path.empty()) return;
    namespace fs = std::filesystem;
    const fs::path p(m_vm.tabs.file.path);

    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return;

    constexpr std::uintmax_t kMaxViewSize = 2u * 1024u * 1024u;
    const std::uintmax_t size = fs::file_size(p, ec);
    const bool truncated = !ec && size > kMaxViewSize;

    std::ifstream in(p, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());
    if (truncated && content.size() > kMaxViewSize)
        content.resize(static_cast<std::size_t>(kMaxViewSize));

    std::vector<std::string> lines;
    std::string cur;
    for (const char c : content) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty() || content.empty()) lines.push_back(std::move(cur));

    // 同步内联 diff 高亮：重新定位该文件会话内修改的区块起始行
    const std::string abs_path = fs::weakly_canonical(p, ec).string();
    m_vm.tabs.file.changes.clear();
    for (const auto& ch : m_vm.tabs.changes.changes) {
        fs::path cp(ch.file_path);
        if (!cp.is_absolute()) cp = fs::current_path() / cp;
        std::error_code ec2;
        if (fs::weakly_canonical(cp, ec2).string() != abs_path) continue;
        FileChange copy = ch;
        const int start = locate_block(lines, ch.new_string);
        // 内容已不存在（手动编辑删除/改写）→ 丢弃旧错位区块，避免高亮错行
        if (start < 0) continue;
        copy.new_start = start + 1;  // 1-based
        m_vm.tabs.file.changes.push_back(std::move(copy));
    }

    // 变更记录联动：编辑前后内容不同 → 生成 FileChange（手动编辑）
    if (m_vm.tabs.file.lines != lines) {
        FileChange ch;
        ch.file_path = abs_path;
        ch.old_string = join_lines(m_vm.tabs.file.lines);
        ch.new_string = join_lines(lines);
        ch.purpose = std::string(str::kEditChangePurpose);
        ch.reasoning = std::string(str::kEditChangeReason);
        ch.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ch.diff = agent::line_diff(m_vm.tabs.file.lines, lines, 1);
        // 定位新内容中的修改区块填充 new_start（否则 file_viewer 跳过渲染）
        const int start = locate_block(lines, ch.new_string);
        if (start >= 0) ch.new_start = start + 1;  // 1-based
        m_vm.tabs.changes.changes.push_back(ch);
        m_vm.tabs.changes_open = true;
        // 手动编辑同步进文件 tab 高亮（编辑前预置的区块基于旧行号已错位）
        m_vm.tabs.file.changes.push_back(std::move(ch));
    }

    m_vm.tabs.file.lines = std::move(lines);
    m_vm.tabs.file.image.reset();
    m_vm.tabs.file.lang = lang_from_path(p.string());
    m_vm.tabs.file.dirty = false;
    if (truncated)
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kViewTooLarge)});
}

/// @brief /Test:askuser：直接用示例 questions 弹出 AskUser 提问弹窗（调试 TUI 渲染/交互）
/// @details 不经 EventBus/AskUserTool（纯 UI 测试），构建 ActionAskUser 直接进模态；
///          关闭时 close_ask 依据 m_ask_test_echo 把答案回显为 assistant 消息。
void App::cmd_test_askuser() {
    static const std::string input_str = R"JSON({
        "questions": [
            {
                "question": "选择一种认证方式？",
                "header": "认证",
                "multiSelect": false,
                "allow_custom_input": true,
                "options": [
                    {"label": "OAuth 2.0", "description": "委托授权（推荐）"},
                    {"label": "API Key", "description": "长期密钥"},
                    {"label": "Basic Auth", "description": "用户名密码"}
                ]
            },
            {
                "question": "要启用哪些功能？",
                "header": "功能",
                "multiSelect": true,
                "allow_custom_input": true,
                "options": [
                    {"label": "代码检索", "description": "跨文件搜索"},
                    {"label": "语法高亮", "description": "树形解析"},
                    {"label": "文件索引", "description": "异步建索引"},
                    {"label": "实时协作", "description": "多端同步"}
                ]
            }
        ]
    })JSON";
    m_ask_test_echo = true;
    auto promise = std::make_shared<std::promise<agent::AskUserResult>>();
    handle_ask_user(ActionAskUser{
        .questions = nlohmann::json::parse(input_str),
        .timeout_ms = 0,          // 不限时，便于慢慢手动测试
        .result_promise = std::move(promise),
        .cancel_flag = nullptr,
    });
    m_screen.PostEvent(Event::Custom);  // 唤醒事件循环重绘模态
}

// ---------------------------------------------------------------------------
// 统一悬浮面板：/resume 会话 · /provider 供应商（复用 search_palette 组件）
// ---------------------------------------------------------------------------

void App::open_resume_palette() {
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_model_open = false;
    m_mode_open = false;
    m_provider_open = false;
    ensure_sessions_loaded();
    m_session_entries.clear();
    for (size_t i = 0; i < m_session_metas.size(); ++i) {
        const auto& s = m_session_metas[i];
        m_session_entries.push_back(SearchEntry{
            .category = SearchCategory::Session,
            .title = s.title,
            .subtitle = s.file_path,
            .keywords = std::to_string(s.message_count) + std::string(str::kMsgCountKeyword),
            .payload = static_cast<int>(i),
        });
    }
    m_resume_open = true;
    if (m_resume_comp) m_resume_comp->TakeFocus();
}

void App::open_provider_palette() {
    log_run("provider: open_provider_palette");
    if (!m_deps.config_manager) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoProviderConfig)});
        return;
    }
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_resume_open = false;
    m_model_open = false;
    m_mode_open = false;
    m_providers = agent::load_provider_configs(*m_deps.config_manager);
    m_current_provider =
        m_deps.config_manager->get_or<std::string>(agent::keys::PROVIDER, "");
    m_provider_open = true;
    if (m_provider_comp) m_provider_comp->TakeFocus();
}

void App::switch_provider(int index) {
    if (index < 0 || index >= static_cast<int>(m_providers.size())) return;
    const agent::ProviderConfigEntry entry = m_providers[static_cast<size_t>(index)];
    log_run("provider: switch_provider index=" + std::to_string(index) +
            " name=" + entry.name + " id=" + entry.id +
            " providers_size=" + std::to_string(m_providers.size()));
    if (!m_deps.create_provider) {
        log_run("provider: switch_provider FAILED no create_provider dep");
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderSwitchFailedPrefix) + entry.name
                    + std::string(str::kCloseParenNl)});
        return;
    }
    // 正在生成中拒绝热切换（ReAct 循环持有 provider；run_completion 期间换后端竞态）
    if (m_deps.session && m_deps.session->is_generating()) {
        log_run("provider: switch_provider REJECTED session generating");
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderBusy) + entry.name
                    + std::string(str::kCloseParenNl)});
        return;
    }
    // 后台创建新后端（不阻塞 UI）；完成后经事件队列回 UI 线程执行热切换
    // 以目标条目自身配置为准创建（base_url/api_key/model），
    // 自定义供应商也无需依赖切换前旧的全局 cfg
    std::thread([this, entry] {
        auto result = m_deps.create_provider(entry);
        if (!result.provider) {
            log_run("provider: create FAILED url=" + entry.base_url +
                    " model=" + result.model_name);
            m_queue.push(ActionProviderSwitchFailed{.provider_name = entry.name});
            m_screen.PostEvent(Event::Custom);
            return;
        }
        log_run("provider: create OK model=" + result.model_name);
        m_queue.push(ActionProviderSwitched{
            .provider = std::move(result.provider),
            .model_name = result.model_name,
            .entry = entry,
        });
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

void App::handle_provider_switched(std::unique_ptr<agent::ICompletionProvider> provider,
                                   const std::string& model_name,
                                   const agent::ProviderConfigEntry& entry) {
    log_run("provider: handle_provider_switched name=" + entry.name);
    // 持久化切换始终执行（即使当前无会话）：apply_provider_switch 只写内存，
    // 必须落盘 provider/remote_url/model，否则重启读取旧配置还原为上一供应商
    if (m_deps.config_manager)
        agent::apply_provider_switch(*m_deps.config_manager, entry);
    if (m_deps.save_config) m_deps.save_config();

    // 无会话（启动时自定义供应商未装配后端 / mock 模式）：仅更新配置与界面显示。
    // 新增后端随之析构；下次启动按新 provider 经 create_session 兜底装配会话。
    if (!m_deps.session) {
        log_run("provider: switched CONFIG-ONLY (no session) name=" + entry.name);
        if (!model_name.empty()) {
            m_vm.sidebar.model = model_name;
            if (m_deps.on_model_changed) m_deps.on_model_changed();
        }
        rebuild_model_entries();
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderSwitchedPrefix) + entry.name
                    + std::string(str::kMdBoldEnd)});
        return;
    }
    // 保留当前对话继续（import_messages 重置上下文压缩基线，不丢历史）
    auto messages = m_deps.session->get_messages();
    if (!m_deps.session->set_provider(std::move(provider))) {
        log_run("provider: set_provider REJECTED session busy");
        // 竞态（处理时已开始生成）：拒绝本次切换，新后端随 unique_ptr 析构
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderBusy) + entry.name
                    + std::string(str::kCloseParenNl)});
        return;
    }
    m_deps.session->import_messages(std::move(messages));
    // 刷新 admin 句柄（旧指针已随旧 provider 失效）
    if (auto* p = m_deps.session->completion_provider())
        m_deps.backend_admin = dynamic_cast<agent::IBackendAdmin*>(p);
    log_run("provider: switched OK name=" + entry.name +
            " model=" + model_name);
    // 更新侧栏模型显示与模型列表 active 标记
    if (!model_name.empty()) {
        m_vm.sidebar.model = model_name;
        if (m_deps.on_model_changed) m_deps.on_model_changed();
    }
    rebuild_model_entries();
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kProviderSwitchedPrefix) + entry.name
                + std::string(str::kMdBoldEnd)});
}

void App::handle_provider_switch_failed(const std::string& provider_name) {
    log_run("provider: switch FAILED name=" + provider_name);
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kProviderSwitchFailedPrefix) + provider_name
                + std::string(str::kCloseParenNl)});
}

// ---------------------------------------------------------------------------
// 输入栏提示面板（/ 命令 · @ 文件）：状态机由 composer 事件驱动
// ---------------------------------------------------------------------------

void App::update_suggest() {
    std::string query;
    const SuggestMode mode = parse_suggest_query(m_input_buffer, query);
    m_suggest_mode = mode;
    m_suggest_entries.clear();
    m_suggest_files.clear();

    if (mode == SuggestMode::Command) {
        // 命令候选：注册表派生条目（与搜索面板共用 m_palette_cmds）
        std::vector<std::string> searchable;
        searchable.reserve(m_palette_cmds.size());
        for (const auto& c : m_palette_cmds)
            searchable.push_back(c.command + " " + c.title + " " + c.keywords);
        for (const size_t i : filter_commands(searchable, query)) {
            m_suggest_entries.push_back(SuggestEntry{
                .title = m_palette_cmds[i].command,
                .subtitle = truncate_palette_text(m_palette_cmds[i].description),
                .payload = static_cast<int>(i),
            });
        }
    } else if (mode == SuggestMode::File) {
        auto& fi = agent::global_file_index();
        if (fi.is_ready()) {
            m_suggest_files = fi.search(query, 15);
            for (size_t i = 0; i < m_suggest_files.size(); ++i) {
                std::string title = m_suggest_files[i].name;
                if (m_suggest_files[i].is_directory) title += "/";
                m_suggest_entries.push_back(SuggestEntry{
                    .title = std::move(title),
                    .subtitle = m_suggest_files[i].relative_path,
                    .payload = static_cast<int>(i),
                });
            }
        }
    }
    // 选中项：过滤后保留（clamp），首次取 0
    if (m_suggest_entries.empty()) {
        m_suggest_selected = -1;
    } else if (m_suggest_selected < 0 ||
               m_suggest_selected >= static_cast<int>(m_suggest_entries.size())) {
        m_suggest_selected = 0;
    }
}

void App::suggest_move(int delta) {
    if (m_suggest_entries.empty()) return;
    const int n = static_cast<int>(m_suggest_entries.size());
    m_suggest_selected = (m_suggest_selected + delta) % n;
    if (m_suggest_selected < 0) m_suggest_selected += n;
}

bool App::suggest_accept() {
    if (m_suggest_mode == SuggestMode::None || m_suggest_selected < 0) return false;
    const auto& e = m_suggest_entries[static_cast<size_t>(m_suggest_selected)];
    if (m_suggest_mode == SuggestMode::Command) {
        // 命令面板：把选中命令「追加」到输入栏（保留 "/" 之前已输入的内容），
        // 关闭面板（不执行、不发送消息）。支持连续追加多个命令，如
        // "/skill-001 + /skill002"。
        if (e.payload >= 0 && e.payload < static_cast<int>(m_palette_cmds.size())) {
            const std::string full =
                m_palette_cmds[static_cast<size_t>(e.payload)].command;
            m_input_buffer = apply_command_suggest(m_input_buffer, full);
        }
        m_composer_cursor = m_input_buffer.size();
    } else if (m_suggest_mode == SuggestMode::File) {
        if (e.payload < 0 || e.payload >= static_cast<int>(m_suggest_files.size())) {
            suggest_cancel();
            return false;
        }
        const auto& f = m_suggest_files[static_cast<size_t>(e.payload)];
        // 输入框：@ 只插入文件引用（nvim 打开走搜索面板 @ 文件项）
        const auto at = m_input_buffer.rfind('@');
        if (at == std::string::npos) {
            suggest_cancel();
            return false;
        }
        m_input_buffer = m_input_buffer.substr(0, at + 1) + f.relative_path + " ";
        m_composer_cursor = m_input_buffer.size();
    }
    suggest_cancel();
    return true;
}

bool App::suggest_enter() { return suggest_accept(); }

bool App::suggest_enter_insert() { return suggest_accept(); }

void App::suggest_cancel() {
    m_suggest_mode = SuggestMode::None;
    m_suggest_entries.clear();
    m_suggest_files.clear();
    m_suggest_selected = -1;
}

// ---------------------------------------------------------------------------
// 全局聚合搜索面板（Ctrl+P）：会话 / 文件 / 功能 / 设置
// ---------------------------------------------------------------------------

namespace {

/// @brief 设置动作（搜索面板「设置」类 payload）
enum class SettingAction {
    ModeCycle,      ///< 切换工作模式（标准 / 计划 / 极简）
    PermCycle,      ///< 切换权限模式（手动审批 / 完全访问）
    ModelSelector,  ///< 打开模型选择器
    ProviderSelector, ///< 打开供应商切换面板
    ToggleSidebar,  ///< 切换侧边栏位置（左 / 右）
    NewSession,     ///< 新建会话
    CompactContext, ///< 压缩上下文
    Clear,          ///< 清空会话
    Exit,           ///< 退出
};

/// @brief 追加一条设置条目
void push_setting(std::vector<SearchEntry>& out, SettingAction action,
                  std::string_view title, std::string_view desc,
                  std::string_view keywords) {
    out.push_back(SearchEntry{
        .category = SearchCategory::Setting,
        .title = std::string(title),
        .subtitle = std::string(desc),
        .keywords = std::string(keywords),
        .payload = static_cast<int>(action),
    });
}

}  // namespace

void App::ensure_sessions_loaded() {
    // 会话：缓存；未加载则后台加载（面板本轮跳过，加载完成自动刷新）
    if (!m_session_metas.empty() || m_sessions_loading || m_deps.session_dir.empty()) return;
    m_sessions_loading = true;
    const std::string dir = m_deps.session_dir;
    std::thread([this, dir] {
        auto sessions = agent::session::SessionStore::list_sessions(dir);
        std::vector<SessionLite> lite;
        lite.reserve(sessions.size());
        for (const auto& s : sessions) {
            lite.push_back(SessionLite{
                .title = s.title.empty() ? s.session_id : s.title,
                .file_path = s.file_path,
                .project_name = s.cwd.empty()
                                    ? std::string()
                                    : std::filesystem::path(s.cwd).filename().string(),
                .message_count = s.message_count,
            });
        }
        m_queue.push(ActionSessionsLoaded{.sessions = std::move(lite)});
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

std::vector<SearchEntry> App::assemble_search_entries() {
    std::vector<SearchEntry> out;
    out.reserve(m_palette_cmds.size() + 32);

    // 功能（命令/skill）：注册表派生命令（含磁盘 skills）；"/" 前缀可单独筛选
    for (size_t i = 0; i < m_palette_cmds.size(); ++i) {
        const auto& c = m_palette_cmds[i];
        out.push_back(SearchEntry{
            .category = SearchCategory::Feature,
            .title = c.command,
            .subtitle = truncate_palette_text(c.description),
            .keywords = c.keywords,
            .payload = static_cast<int>(i),
        });
    }

    // 文件：最近修改（索引就绪后）
    auto& fi = agent::global_file_index();
    if (fi.is_ready()) {
        const auto files = fi.search("", 15);
        for (size_t i = 0; i < files.size(); ++i) {
            out.push_back(SearchEntry{
                .category = SearchCategory::File,
                .title = files[i].name,
                .subtitle = files[i].relative_path,
                .payload = static_cast<int>(i),
            });
        }
    }

    // 会话：缓存；未加载则后台加载（面板本轮跳过，加载完成自动刷新）
    ensure_sessions_loaded();
    for (size_t i = 0; i < m_session_metas.size(); ++i) {
        const auto& s = m_session_metas[i];
        out.push_back(SearchEntry{
            .category = SearchCategory::Session,
            .title = s.title,
            .subtitle = s.project_name,
            .keywords = std::to_string(s.message_count) + std::string(str::kMsgCountKeyword),
            .payload = static_cast<int>(i),
        });
    }

    // 设置：静态条目
    push_setting(out, SettingAction::ModeCycle, str::kSettingMode, str::kSettingModeDesc,
                 "mode 模式 标准 计划 极简");
    push_setting(out, SettingAction::PermCycle, str::kSettingPerm, str::kSettingPermDesc,
                 "permission bypass 权限 审批 访问");
    push_setting(out, SettingAction::ModelSelector, str::kSettingModel, str::kSettingModelDesc,
                 "model 模型");
    push_setting(out, SettingAction::ProviderSelector, str::kSettingProvider,
                 str::kSettingProviderDesc, "provider 供应商");
    push_setting(out, SettingAction::NewSession, str::kSettingNewSession,
                 str::kSettingNewSessionDesc, "new session 新建 会话");
    push_setting(out, SettingAction::CompactContext, str::kSettingCompact,
                 str::kSettingCompactDesc, "compact 压缩 上下文");
    push_setting(out, SettingAction::ToggleSidebar, str::kSettingSidebar,
                 str::kSettingSidebarDesc, "sidebar side 侧边栏 位置");
    push_setting(out, SettingAction::Clear, str::kSettingClear, str::kSettingClearDesc,
                 "clear 清空");
    push_setting(out, SettingAction::Exit, str::kSettingExit, str::kSettingExitDesc,
                 "exit quit 退出");

    return out;
}

void App::apply_search_entry(int index) {
    if (index < 0 || index >= static_cast<int>(m_search_entries.size())) return;
    const SearchEntry& e = m_search_entries[static_cast<size_t>(index)];
    switch (e.category) {
        case SearchCategory::Feature: {
            // 执行命令（含 skill）：经统一命令路径
            if (e.payload >= 0 && e.payload < static_cast<int>(m_palette_cmds.size()))
                run_command(m_palette_cmds[static_cast<size_t>(e.payload)].command, "");
            break;
        }
        case SearchCategory::File: {
            // 打开 nvim 编辑选中文件（搜索面板 @ 搜索 → 直接编辑）
            auto& fi = agent::global_file_index();
            if (fi.is_ready()) {
                const auto files = fi.search("", 15);
                if (e.payload >= 0 && e.payload < static_cast<int>(files.size())) {
                    const auto& f = files[static_cast<size_t>(e.payload)];
                    cmd_edit(f.relative_path);
                }
            }
            break;
        }
        case SearchCategory::Session:
            if (e.payload >= 0 && e.payload < static_cast<int>(m_session_metas.size())) {
                const auto& s = m_session_metas[static_cast<size_t>(e.payload)];
                resume_session(s.file_path, s.title);
            }
            break;
        case SearchCategory::Setting:
            run_setting(e.payload);
            break;
    }
}

void App::run_setting(int action) {
    switch (static_cast<SettingAction>(action)) {
        case SettingAction::ModeCycle:
            open_mode_selector();  // 模式选择面板（与 /model 同款，含模式介绍）
            break;
        case SettingAction::PermCycle:
            toggle_permission();
            break;
        case SettingAction::ModelSelector:
            open_model_selector();
            break;
        case SettingAction::ProviderSelector:
            open_provider_palette();
            break;
        case SettingAction::NewSession:
            cmd_new();
            break;
        case SettingAction::CompactContext:
            compact_context();
            break;
        case SettingAction::ToggleSidebar:
            m_sidebar_left = !m_sidebar_left;
            m_screen.RequestAnimationFrame();
            break;
        case SettingAction::Clear:
            cmd_clear();
            break;
        case SettingAction::Exit:
            m_vm.apply(ActionShutdown{});
            if (m_vm.pending_exit) m_screen.Exit();
            break;
    }
}

// —— 技能命令任意位置调用 ——
namespace {
/// /name 后边界判定：name 之后第一个字符（input[pos]）
/// 到行尾 / 空白 / 标点 / 非 ASCII（含 CJK）→ 视为参数起始（边界成立）；
/// ASCII 字母数字 - _ 视为名字延续（非边界，避免正则 "/verify" 误吞 "/verify-check"）。
bool is_skill_boundary_after(const std::string& input, std::size_t pos) {
    if (pos >= input.size()) return true;
    const unsigned char c = static_cast<unsigned char>(input[pos]);
    if (c >= 0x80) return true;
    const bool word = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                      (c >= 'A' && c <= 'Z') || c == '-' || c == '_';
    return !word;
}

/// 在 input 任意位置查找首个已注册技能命令 "/name"，返回命中信息。
/// 技能判定：命令注册表中 type()=="prompt"（内置命令均为 local，技能为 prompt）。
struct SkillHit {
    std::string name;
    std::string args;  ///< /name 之后到行尾的参数文本
};
std::optional<SkillHit> find_skill_command_anywhere(
    const std::string& input, const agent::command::CommandRegistry& reg) {
    std::vector<std::string> names;
    for (const auto& c : reg.get_user_invocable_commands())
        if (c->type() == "prompt") names.push_back(c->name());
    if (names.empty() || input.empty()) return std::nullopt;
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '/') continue;
        for (const auto& n : names) {
            if (input.compare(i + 1, n.size(), n) != 0) continue;
            if (is_skill_boundary_after(input, i + 1 + n.size())) {
                return SkillHit{n, input.substr(i + 1 + n.size())};
            }
        }
    }
    return std::nullopt;
}
}  // namespace

void App::send_input(const std::string& text, bool force_flush) {
    // 输入历史：提交即追加并落盘（含斜杠命令，便于重复调用）
    if (!text.empty()) {
        m_input_history.push(text);
        m_input_history.save();
    }

    // ！命令：以 '!' 开头视为 Shell 命令执行（跨平台 cmd/sh）。
    // 顺序置于技能/斜杠命令之前，避免命令串内含 '/...' 被误判为技能调用。
    // Enter 只执行并渲染 Bash 卡；Ctrl+Enter（force_flush）再把结果结构化发给模型。
    if (!text.empty() && text[0] == '!') {
        run_shell_command(text, force_flush);
        return;
    }

    // 技能命令任意位置调用：/skill-name 可出现在消息任何位置（含开头）。
    // 命中即：回显原始输入 + 注入合成 Skill 卡片 + 本地解析后路由模型。
    if (!text.empty() && m_deps.command_registry) {
        if (auto hit = find_skill_command_anywhere(text, *m_deps.command_registry)) {
            handle_skill_invocation(text, hit->name, hit->args);
            return;
        }
    }

    // 本地命令：不发送给模型（技能已在上方拦截，此处剩余为非技能斜杠命令）
    if (!text.empty() && text[0] == '/') {
        // B2 统一命令：斜杠命令全部经 run_command 执行（单一命令路径）
        run_command(text, "");
        return;
    }

    // @图片引用（如 @a.png）→ 图片附件绝对路径（多模态随请求上传）：
    // 仅普通文本路径提取，非图片 @ 文件引用保持原样发送（模型经工具读取）。
    auto extract_images = [&]() -> std::vector<std::string> {
        std::vector<std::string> images;
        agent::input::InputParser parser;
        const auto parsed = parser.parse(text);
        if (parsed.type == agent::input::InputType::Text) {
            for (const auto& p : parsed.image_paths) {
                std::error_code ec;
                const auto abs = std::filesystem::weakly_canonical(
                    std::filesystem::absolute(p, ec), ec);
                if (!ec && !abs.empty() && std::filesystem::exists(abs, ec)) {
                    images.push_back(abs.string());
                }
            }
        }
        return images;
    };

    // 模型忙碌：进入待发送队列（不丢消息）；Ctrl+Enter 请求下个工具轮边界立即冲刷。
    // 不回显到转录区（由输入框上方队列卡片展示），不重复置 busy（已在生成中）。
    if (m_deps.session && m_deps.session->is_generating()) {
        if (force_flush) {
            m_deps.session->request_flush();
        }
        if (m_deps.on_submit) {
            // ChatSession::send_message 忙碌时自动入队 + 发布 MessageQueueUpdatedEvent
            m_deps.on_submit(text, extract_images());
            // 唤醒事件循环消费队列更新事件（事件总线仅入队、无泵线程）
            m_screen.PostEvent(Event::Custom);
        }
        return;
    }

    // 回显用户消息
    m_vm.apply(ActionAppendMessage{.role = "user", .text = text});
    m_vm.apply(ActionSetBusy{.busy = true});
    m_follow = true;

    if (m_deps.mock_mode) {
        start_mock_stream(text);
        return;
    }

    // 真实链路：统一经 on_submit 路由到会话（B2：输入链单一入口）
    if (m_deps.on_submit) {
        m_deps.on_submit(text, extract_images());
        // 真实链路唤醒：busy 已置位但动画线程镜像同步发生在 drain（Custom 事件）。
        // 立即投递一次 Custom 让 UI 线程消费事件队列并唤醒动画线程；
        // 否则 publish_async 事件积压（事件总线仅入队、无泵线程），
        // 界面停在"生成中"直到用户再次按键（而按键也不触发 drain）。
        m_screen.PostEvent(Event::Custom);
    } else {
        m_vm.apply(ActionSetBusy{.busy = false});
    }
}

void App::handle_skill_invocation(const std::string& raw_input,
                                  const std::string& name,
                                  const std::string& args) {
    // 1. 回显用户原始输入（含 /skill-name 指令文本）
    m_vm.apply(ActionAppendMessage{.role = "user", .text = raw_input});
    m_vm.apply(ActionSetBusy{.busy = true});
    m_follow = true;

    if (!m_command_processor) {
        m_vm.apply(ActionAppendSkill{.name = name, .input = args, .is_error = true});
        m_vm.apply(ActionAppendMessage{.role = "assistant",
                                       .text = std::string(str::kProcessorUnavailable)});
        return;
    }

    // 2. 本地解析技能 → 展开提示词
    std::string invoke = "/" + name + (args.empty() ? "" : (" " + args));
    agent::command::CommandContext ctx;
    auto result = m_command_processor->process(invoke, ctx);

    // 计算实际发往模型的展开提示词（/resume 时用于定位对应 user 消息）
    std::string query;
    if (result.should_query) {
        query = result.output_text;
        if (query.empty()) {
            for (const auto& m : result.messages) {
                if (!query.empty()) query += "\n\n";
                query += m;
            }
        }
    }

    // 注入合成 Skill 卡片（本地解析完成态；仅 ViewModel 展示，不进模型上下文）
    m_vm.apply(ActionAppendSkill{.name = name, .input = args, .is_error = result.is_error});

    // skill 事件载荷（/resume 按 query 匹配恢复为「原始输入回显 + Skill 卡」）
    const agent::session::SkillEvent skill_ev{
        .name = name,
        .input = args,
        .raw_input = raw_input,
        .query = query,
        .is_error = result.is_error,
    };
    // 落盘（须在 user 消息持久化之后：SessionStore 在首条 user 消息时懒创建，
    // 若 skill 是首条消息且先于 on_submit 落盘，store 尚不存在会被静默丢弃 → /resume 恢复失败）
    auto persist_skill = [this](const agent::session::SkillEvent& ev) {
        if (m_deps.session) m_deps.session->append_skill_event(ev);
    };

    if (result.is_error) {
        persist_skill(skill_ev);
        if (!result.output_text.empty()) m_vm.apply(ActionError{.message = result.output_text});
        return;
    }

    // 4. 本地命令型技能（should_query=false）直接输出
    if (!result.output_text.empty() && !result.should_query) {
        persist_skill(skill_ev);
        m_vm.apply(ActionAppendMessage{.role = "assistant", .text = result.output_text});
        return;
    }

    // 5. 需要模型：先把展开后的提示词交给 on_submit（同步持久化 user 消息、创建 store），
    //    再落盘 skill 事件（保证事件晚于其 query 消息，/resume 按 query 内容匹配即可恢复）。
    if (result.should_query && m_deps.on_submit) {
        if (!query.empty()) {
            m_deps.on_submit(query, {});
            persist_skill(skill_ev);
            m_screen.PostEvent(Event::Custom);  // 同 run_command：唤醒事件循环消费积压事件
        }
    }
}

void App::run_command(const std::string& cmd, const std::string& args) {
    // 组装完整命令串（args 已含空格分隔；若调用方传了原始文本则整体处理）
    std::string input = args.empty() ? cmd : cmd + " " + args;
    if (!m_command_processor) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
                                       .text = std::string(str::kProcessorUnavailable)});
        return;
    }

    agent::command::CommandContext ctx;
    auto result = m_command_processor->process(input, ctx);

    if (result.is_error) {
        m_vm.apply(ActionError{.message = result.output_text});
        return;
    }
    // 本地命令有输出文本 → 追加为 assistant 消息（不回显到模型，token 统计为 0）
    if (!result.output_text.empty() && !result.should_query) {
        m_vm.apply(ActionAppendMessage{.role = "assistant", .text = result.output_text});
        return;
    }
    // 需要调模型：交给 on_submit 路由到会话（与普通文本一致）
    if (result.should_query && m_deps.on_submit) {
        std::string query = result.output_text;
        if (query.empty()) {
            for (const auto& m : result.messages) {
                if (!query.empty()) query += "\n\n";
                query += m;
            }
        }
        if (!query.empty()) {
            m_vm.apply(ActionAppendMessage{.role = "user", .text = query});
            m_vm.apply(ActionSetBusy{.busy = true});
            m_deps.on_submit(query, {});
            m_screen.PostEvent(Event::Custom);  // 同 send_input：唤醒事件循环消费积压事件
        }
    }
}

namespace {

/// @brief 跨平台执行一条 Shell 命令，复用 shell_detect::detect()。
/// @details Windows 优先 Git Bash（bash -c），未安装则降级 cmd.exe（/c）；
///          POSIX 走 /bin/sh -c。与 BashTool 共用同一 shell 判定，保证命令语义一致，
///          例如 `!ls`、`!grep` 在安装 Git Bash 的 Windows 上可直接执行。
///          设超时防命令挂死。
agent::ResultV2<agent::process::ExecOutput> exec_shell_command(
    const std::string& command) {
    // 引用静态缓存，程序生命周期内有效（shell_detect::detect 已做线程安全缓存）
    const auto& sh = agent::tool::shell_detect::detect();
    agent::process::ExecOptions opts;
    opts.args = {sh.flag, command};
    opts.timeout = std::chrono::milliseconds(120000);
    return agent::process::exec(sh.cmd, opts);
}

/// @brief 组装发给模型的结构化命令结果（用户执行了什么命令、结果如何）
std::string build_cmd_submit_text(
    const std::string& command,
    const agent::ResultV2<agent::process::ExecOutput>& res) {
    std::string out = "用户执行了 Shell 命令：\n```bash\n" + command + "\n```\n";
    if (res.is_err()) {
        out += "\n命令启动失败：" + res.error().message;
    } else {
        const auto& r = res.value();
        if (r.cancelled) {
            out += "\n命令执行已被取消。";
        } else if (r.timed_out) {
            out += "\n命令执行超时。";
        } else {
            out += "\n命令退出码：" + std::to_string(r.exit_code) + "\n";
            if (!r.stdout_text.empty())
                out += "\n标准输出：\n" + r.stdout_text + "\n";
            if (!r.stderr_text.empty())
                out += "\n标准错误：\n" + r.stderr_text + "\n";
        }
    }
    return out;
}

}  // namespace

void App::run_shell_command(const std::string& raw_input, bool send_to_model) {
    std::string command = raw_input.substr(1);  // 去前导 '!'
    const size_t b = command.find_first_not_of(" \t\n\r");
    const size_t e = command.find_last_not_of(" \t\n\r");
    if (b == std::string::npos) return;         // 空命令：忽略
    command = command.substr(b, e - b + 1);

    // 回显用户输入（含 '!' 前缀）
    m_vm.apply(ActionAppendMessage{.role = "user", .text = raw_input});
    if (m_deps.mock_mode) return;  // mock 模式不真实执行（合成结果卡由 mock 演示）

    // 串行执行：上一命令未结束则等待，避免并发命令 push 交错
    if (m_cmd_thread.joinable()) m_cmd_thread.join();

    m_cmd_thread = std::thread([this, command, send_to_model] {
        auto res = exec_shell_command(command);
        std::string result;
        bool is_error = false;
        if (res.is_err()) {
            result = std::string("<error>\n命令启动失败：") + res.error().message + "\n</error>";
            is_error = true;
        } else {
            const auto& out = res.value();
            if (out.cancelled || out.timed_out) {
                result = std::string("<error>\n命令") +
                         (out.timed_out ? "执行超时" : "执行已被取消") + "\n</error>";
                is_error = true;
            } else {
                if (out.stdout_text.empty()) {
                    result = "<stdout>\n</stdout>\n";
                } else {
                    result = "<stdout>\n" + out.stdout_text + "\n</stdout>\n";
                }
                if (!out.stderr_text.empty())
                    result += "<stderr>\n" + out.stderr_text + "\n</stderr>\n";
                result += "<exit_code>" + std::to_string(out.exit_code) + "</exit_code>";
                is_error = (out.exit_code != 0);
            }
        }
        m_queue.push(ActionAppendCmdResult{
            .command = command, .result = std::move(result), .is_error = is_error});
        if (send_to_model) {
            m_queue.push(ActionSubmitCmdToModel{
                .text = build_cmd_submit_text(command, res)});
        }
        // 唤醒 UI 线程消费队列并重绘（模拟流/项目扫描/轮询线程同一模式）
        m_screen.PostEvent(ftxui::Event::Custom);
    });
}

void App::start_mock_stream(const std::string& user_text) {
    // 清理可能残留的旧流
    if (m_stream_thread.joinable()) {
        m_stream_run = false;
        m_stream_thread.join();
    }

    const std::string reply = build_mock_reply(user_text);
    const std::string reasoning = build_mock_reasoning(user_text);
    log_run("mock: stream start reply_len=" + std::to_string(reply.size()) +
            " reasoning_len=" + std::to_string(reasoning.size()));
    m_stream_run = true;
    m_stream_thread = std::thread([this, reply, reasoning] {
        // 思考阶段：推送 reasoning 增量（思考中动画由 UI 帧驱动），计时用于折叠标签
        const auto t0 = std::chrono::steady_clock::now();
        constexpr size_t kThinkChunk = 6;
        for (size_t i = 0; i < reasoning.size() && m_stream_run; i += kThinkChunk) {
            m_queue.push(ActionReasoningDelta{.delta = reasoning.substr(i, kThinkChunk)});
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        const auto think_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        log_run("mock: reasoning done think_ms=" + std::to_string(think_ms));
        // 工具调用演示：read_file + grep（模拟 ReAct 单步）
        const auto push_tool = [this](const std::string& name, ActionBeginTool begin,
                                      ActionEndTool end, int run_ms) {
            log_run("mock: tool begin " + name);
            m_queue.push(std::move(begin));
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(run_ms));
            m_queue.push(std::move(end));
            m_screen.PostEvent(Event::Custom);
            log_run("mock: tool end " + name);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        };
        push_tool("read_file",
            ActionBeginTool{.tool_name = "read_file",
                            .call_id = "mock-read",
                            .arguments = "{\"path\": \"README.md\"}"},
            ActionEndTool{.call_id = "mock-read",
                          .result = "```text\n# Workx\n\nFTXUI 实验 TUI：\n"
                                    "- 折叠卡片（思考 / 工具）\n"
                                    "- 点击头部展开 / 收起\n"
                                    "- Nerd Font 图标\n```\n"},
            900);
        push_tool("grep",
            ActionBeginTool{.tool_name = "grep",
                            .call_id = "mock-grep",
                            .arguments = "{\"pattern\": \"TODO\", \"path\": \"src/\"}"},
            ActionEndTool{.call_id = "mock-grep",
                          .result = "src/app.cpp:12:  // TODO: 待确认\n"
                                    "src/tui/main.cpp:30:  // TODO: 侧栏折叠\n"},
            700);
        // 正文阶段：每 tick 推送一小段，模拟 LLM 流式输出
        constexpr size_t kChunk = 3;
        for (size_t i = 0; i < reply.size() && m_stream_run; i += kChunk) {
            m_queue.push(ActionTokenDelta{.content_delta = reply.substr(i, kChunk)});
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        // 结束：封口并回到 IDLE（reasoning_ms 带思考耗时）
        m_queue.push(ActionTurnDone{.full_content = reply, .prompt_ms = think_ms,
                                    .reasoning_ms = think_ms});
        m_screen.PostEvent(Event::Custom);
        log_run("mock: turn done");
        m_stream_run = false;
        log_run("mock: stream exit");
    });
}

// ---------------------------------------------------------------------------
// 转录布局（A3：单一布局源 — 高度估算与渲染共用 estimate_message_height）
// ---------------------------------------------------------------------------

namespace {

/// @brief 定高留白（O(1)：仅占位，不进行 Markdown 解析/高亮）
ftxui::Element pad_rows(int rows) {
    return ftxui::emptyElement() | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, std::max(0, rows));
}

/// @brief 内容敏感指纹：混入消息/工具参数的实际内容，杜绝"长度相同但内容不同"
///        的 sealed 元素缓存误命中。正文/思考/工具名/参数做全量哈希，工具结果
///        大文本做均匀采样，平衡正确性与长文本开销。
std::uint64_t content_fingerprint(const MessageNode& m) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    const auto mix_full = [&](const std::string& s) {
        mix(s.size());
        for (unsigned char c : s) mix(c);
    };
    const auto mix_sample = [&](const std::string& s) {
        mix(s.size());
        const std::size_t n = s.size();
        if (n == 0) return;
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        const std::size_t step = std::max<std::size_t>(1, n / 32);
        for (std::size_t i = 0; i < n; i += step) mix(p[i]);
        mix(p[n - 1]);
    };
    mix_full(m.text);
    mix_full(m.reasoning);
    for (const auto& t : m.tool_calls) {
        mix_full(t.tool_name);
        mix_full(t.call_id);
        mix_full(t.arguments);
        mix_sample(t.result);
    }
    return h;
}

}  // namespace

void App::invalidate_msg_cache() {
    m_msg_cache.clear();
    m_msg_height.clear();
    m_msg_height_ver.clear();
}

std::uint64_t App::height_fingerprint(const MessageNode& m) {
    // 与 estimate_message_height 相关的字段；不含宽度（高度与宽度无关）。
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(m.sealed ? 1 : 0);
    mix(m.reasoned ? 1 : 0);
    mix(m.reasoning_expanded ? 1 : 0);
    mix((std::uint64_t)m.text.size());
    mix((std::uint64_t)m.reasoning.size());
    for (const auto& t : m.tool_calls) {
        mix(t.done ? 1 : 0);
        mix(t.running ? 1 : 0);
        mix(t.is_error ? 1 : 0);
        mix(t.expanded ? 1 : 0);
        mix((std::uint64_t)t.arguments.size());
        mix((std::uint64_t)t.result.size());
    }
    return h;
}

std::uint64_t App::sealed_cache_key(const MessageNode& m, int width) {
    // 渲染指纹 = 高度指纹再并入宽度（resize 时元素需重建）
    std::uint64_t h = height_fingerprint(m) ^ (std::uint64_t)(std::uint32_t)width;
    // 混入内容指纹：防止「长度/折叠相同但内容不同」的消息复用旧渲染树
    // （旧树的 reflect 卡片 box 指向失效位置，曾在工具调用场景触发悬空崩溃）
    h ^= content_fingerprint(m);
    // 确保与「无宽度」的高度指纹严格区分，避免高度缓存误命中元素缓存
    return h ^ 0x6a09e667f3bcc909ull;
}

Element App::build_transcript(int width) {
    const auto& msgs = m_vm.messages;
    const std::size_t n = msgs.size();
    if (m_msg_cache.size() < n) m_msg_cache.resize(n);
    if (m_msg_height.size() < n) {
        m_msg_height.resize(n, -1);
        m_msg_height_ver.resize(n, 0);
    }

    // 逐消息估计高度：sealed 消息复用缓存（指纹未变跳过 estimate），
    // 流式消息逐帧重估。再据此线性扫一遍前缀和（O(n) 整数累加，远轻于
    // 逐条 Markdown 解析）。prefix[k] = 前 k 条消息（含每条后的 1 行间距）。
    // 注：正文按显示列宽折行，高度随 width 变化；sealed 高度指纹混入 width，
    //     终端 resize 时自动重算（元素缓存 sealed_cache_key 亦含 width）。
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = msgs[i];
        if (!m.sealed) {
            m_msg_height[i] = estimate_message_height(m, width);
            m_msg_height_ver[i] = 0;
        } else {
            const std::uint64_t fp = height_fingerprint(m)
                ^ static_cast<std::uint64_t>(width);
            if (m_msg_height_ver[i] != fp) {
                m_msg_height[i] = estimate_message_height(m, width);
                m_msg_height_ver[i] = fp;
            }
        }
    }
    std::vector<int> prefix(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + m_msg_height[i] + 1;

    int dimy = ftxui::Terminal::Size().dimy;
    // 减去：标题 1 + 面包屑 1 + 输入区 composer_height + 状态行 1 + hook 进度条 + 余量 2
    int avail = std::max(1, dimy - (2 + 1 + composer_height(m_input_buffer) + 2
                                   + hook_progress_height()));
    int content_h = prefix[n];
    int max_scroll = std::max(0, content_h - avail);

    // 确定视口顶行：跟随则钉底部，手动则钳制到 [0, max_scroll]
    int scroll_top;
    if (m_follow) {
        scroll_top = max_scroll;
    } else {
        m_scroll = std::max(0, std::min(m_scroll, max_scroll));
        scroll_top = m_scroll;
        if (m_scroll >= max_scroll) {
            // 已滚到底部：恢复自动跟随（新消息到来时继续钉在底部）
            m_follow = true;
        }
    }

    // 只装配可视切片：顶/底用定高留白补齐，保持总高度与滚动范围不变
    m_hits.clear();
    const int margin = std::max(8, avail / 2);
    TranscriptSlice slice = select_transcript_slice(prefix, scroll_top, avail, margin);

    Element content;
    if (slice.empty) {
        // 空转录或卷过底部：整段留白，保持内容高度以维持滚动范围
        content = pad_rows(content_h);
    } else {
        Elements es;
        if (slice.top_pad > 0) es.push_back(pad_rows(slice.top_pad));
        for (std::size_t i = slice.first; i <= slice.last; ++i) {
            const auto& m = msgs[i];
            const std::size_t hit_start = m_hits.size();

            ftxui::Element msg_el;
            const std::deque<CardHit>* cached_hits = nullptr;
            if (m.sealed) {
                // sealed：按指纹复用渲染树，跳过 Markdown 解析/语法高亮
                const auto key = sealed_cache_key(m, width);
                auto& c = m_msg_cache[i];
                if (c.has && c.width == width && c.key == key) {
                    msg_el = c.element;
                    cached_hits = c.hits.get();
                } else {
                    if (!c.hits) c.hits = std::make_unique<std::deque<CardHit>>();
                    c.hits->clear();
                    c.element = build_message(m, width, 0, c.hits.get());
                    c.width = width;
                    c.key = key;
                    c.has = true;
                    msg_el = c.element;
                    cached_hits = c.hits.get();
                }
            } else {
                // 流式未封口：逐帧重建（命中反射当前帧坐标）
                msg_el = build_message(m, width, m_anim_frame, &m_hits);
            }

            if (!msg_el) {
                // 取证：正常情况下不会为空；一旦触发说明某条消息的渲染树失效。
                // 记录现场后用 emptyElement 兜底，避免布局期解引用空节点崩溃。
                LOG_WARN("[transcript] msg_el null! i={} sealed={} stream={} text_len={} "
                         "reasoning_len={} tool_calls={} width={} m_anim_frame={}",
                         i, m.sealed, m.streaming, m.text.size(), m.reasoning.size(),
                         m.tool_calls.size(), width, m_anim_frame);
                msg_el = ftxui::emptyElement();
            }
            es.push_back(ftxui::hbox({
                ftxui::text("  "),
                ftxui::flex(std::move(msg_el)),
            }));

            if (cached_hits) {
                // 缓存卡片的 box 由 reflect 于渲染期回写，滚动时坐标随之刷新
                for (const auto& h : *cached_hits) {
                    CardHit c = h;
                    c.msg_idx = static_cast<int>(i);
                    m_hits.push_back(c);
                }
            } else {
                for (std::size_t k = hit_start; k < m_hits.size(); ++k)
                    m_hits[k].msg_idx = static_cast<int>(i);
            }
            // 消息间空一行；与 prefix 的「每条 +1 间距」对齐
            es.push_back(ftxui::text(" "));
        }
        if (slice.bottom_pad > 0) es.push_back(pad_rows(slice.bottom_pad));
        content = ftxui::vbox(std::move(es));
    }

    if (m_follow) {
        // 跟随：钉在实际内容底部（focusPositionRelative 无估算误差）
        m_scroll = max_scroll;  // 维护估计底部，切到手动滚动时平滑衔接
        return content | ftxui::focusPositionRelative(0, 1.0f) | ftxui::yframe;
    }

    // 手动：+avail/2 抵消 frame 的居中，使 m_scroll 即视口顶行（增大=向下=更新）
    return content
        | ftxui::focusPosition(0, m_scroll + avail / 2)
        | ftxui::yframe;
}

/// @brief 标题栏下的层级子列表（面包屑导航）：主会话 / 子 Agent 记录
/// @details 用 `>` 箭头表示当前所处的层级。点击「主会话」或「子 Agent」项可切换输出层级。
Element App::build_breadcrumb() {
    m_breadcrumb_hits.clear();
    Elements crumbs;

    // 主会话（始终存在，可点击返回）：显示当前会话标题
    {
        // 先占位，取容器内稳定元素（deque 不因扩容失效），由 reflect 于渲染期回写 box
        m_breadcrumb_hits.push_back(CardHit{});
        CardHit& hit = m_breadcrumb_hits.back();
        hit.tool_idx = -1;
        hit.msg_idx = -1;
        hit.nav_target = 0;
        const std::string& title =
            m_vm.sidebar.title.empty() ? std::string(str::kSidebarNewSession) : m_vm.sidebar.title;
        const bool active = (m_vm.output_level == OutputLevel::Main);
        auto el = ftxui::text(title)
            | ftxui::color(active ? theme::T::Text : theme::T::TextDim)
            | ftxui::reflect(hit.box);
        if (active) el = el | ftxui::bold;
        crumbs.push_back(std::move(el));
    }

    if (m_vm.output_level == OutputLevel::SubAgent) {
        // 分隔箭头 + 子 Agent 记录项
        crumbs.push_back(ftxui::text(std::string(str::kOutputSep)) | ftxui::color(theme::T::TextFaint));
        m_breadcrumb_hits.push_back(CardHit{});
        CardHit& hit = m_breadcrumb_hits.back();
        hit.tool_idx = -1;
        hit.msg_idx = -1;
        hit.nav_target = 1;
        std::string label = std::string(str::kOutputSubAgent);
        auto el = ftxui::text(label)
            | ftxui::color(theme::T::Accent)
            | ftxui::reflect(hit.box)
            | ftxui::bold;
        crumbs.push_back(std::move(el));
    }

    auto bar = ftxui::hbox({
        ftxui::text("  "),
        ftxui::hbox(std::move(crumbs)),
        ftxui::flex(ftxui::text("")),
        ftxui::text(std::string(str::kOutputHint)) | ftxui::color(theme::T::TextFaint),
        ftxui::text("  "),
    });
    return bar | ftxui::bgcolor(theme::T::Surface) | ftxui::color(theme::T::TextDim);
}

/// @brief 第二层：子 Agent 独立记录渲染（不混入主转录区）
/// @details 渲染当前查看子 Agent 的步骤序列（思考/工具/观察/最终），
///          复用主会话 build_message 的卡片渲染（思考卡/工具卡/最终答复），
///          独立滚动（m_sub_scroll/m_sub_follow），返回主会话用面包屑/Esc。
///          与主转录区同机制：记录分解为虚拟消息（思考卡/工具卡/最终答复），
///          sealed 步骤按指纹缓存渲染树与高度，滚动时只装配可视切片
///          （select_transcript_slice + pad_rows），避免大记录滚动卡顿。
Element App::build_sub_agent_view(int width) {
    const int idx = m_vm.sub_active;
    if (idx < 0 || static_cast<std::size_t>(idx) >= m_vm.sub_records.size()) {
        m_sub_hits.clear();
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kSubNoRecord)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }
    const auto& rec = m_vm.sub_records[static_cast<std::size_t>(idx)];

    // 切换记录时整体失效第二层缓存（虚拟消息下标随记录变化）
    if (m_sub_cache_task != rec.task_id) {
        m_sub_cache.clear();
        m_sub_height.clear();
        m_sub_height_ver.clear();
        m_sub_cache_task = rec.task_id;
    }

    // ---- 分解为虚拟消息（每步一条，复用主会话 build_message 卡片渲染）----
    std::vector<MessageNode> vmsgs;
    std::vector<int> vmap;  // 虚拟消息下标 → rec.steps 下标（-1 = 思考合并 / 最终答复）
    const bool sealed_all = (rec.status != "running");

    // 思考：合并所有 thought 步骤文本（与主会话单思考块对齐）
    std::string reasoning;
    double reasoning_ms = 0.0;
    for (const auto& s : rec.steps) {
        if (s.step_type == "thought" && !s.thought_text.empty()) {
            if (!reasoning.empty()) reasoning += "\n\n";
            reasoning += s.thought_text;
            reasoning_ms += s.duration_ms;
        }
    }
    if (!reasoning.empty()) {
        MessageNode tnode;
        tnode.role = MsgRole::Assistant;
        tnode.sealed = sealed_all;  // 运行中思考卡显示旋转动画
        tnode.reasoned = true;
        tnode.reasoning = reasoning;
        tnode.reasoning_expanded = rec.reasoning_expanded;
        tnode.reasoning_ms = reasoning_ms;
        vmsgs.push_back(std::move(tnode));
        vmap.push_back(-1);
    }

    // 工具：action 步骤（observation 已合并）→ 工具卡；独立 observation 兜底
    for (std::size_t i = 0; i < rec.steps.size(); ++i) {
        const auto& s = rec.steps[i];
        if (s.step_type != "action" && s.step_type != "observation") continue;
        MessageNode tnode;
        tnode.role = MsgRole::Assistant;
        tnode.sealed = s.done;  // 已关联 observation → 内容定稿可缓存
        ToolCallNode t;
        t.tool_name = s.step_type == "action" ? s.tool_name : "?";
        t.arguments = s.tool_input;
        t.result = s.observation;
        t.running = !s.done;
        t.done = s.done;
        t.is_error = s.is_error;
        t.expanded = s.expanded;
        t.text_pos = 0;  // 工具卡统一排在最终答复之前
        tnode.tool_calls.push_back(std::move(t));
        vmsgs.push_back(std::move(tnode));
        vmap.push_back(static_cast<int>(i));
    }

    // 最终答复
    if (!rec.final_answer.empty()) {
        MessageNode fnode;
        fnode.role = MsgRole::Assistant;
        fnode.sealed = sealed_all;
        fnode.text = rec.final_answer;
        vmsgs.push_back(std::move(fnode));
        vmap.push_back(-1);
    }

    // ---- 高度估算 + 前缀和（sealed 复用缓存，流式逐帧重估）----
    const std::size_t n = vmsgs.size();
    if (m_sub_cache.size() < n) m_sub_cache.resize(n);
    if (m_sub_height.size() < n) {
        m_sub_height.resize(n, -1);
        m_sub_height_ver.resize(n, 0);
    }
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = vmsgs[i];
        if (!m.sealed) {
            m_sub_height[i] = estimate_message_height(m, width);
            m_sub_height_ver[i] = 0;
        } else {
            const std::uint64_t fp = height_fingerprint(m)
                ^ static_cast<std::uint64_t>(width);
            if (m_sub_height_ver[i] != fp) {
                m_sub_height[i] = estimate_message_height(m, width);
                m_sub_height_ver[i] = fp;
            }
        }
    }
    std::vector<int> prefix(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + m_sub_height[i] + 1;

    // 视口高度：与主转录区同公式，再减状态头 2 行（头 + 空行）
    int dimy = ftxui::Terminal::Size().dimy;
    int avail = std::max(1, dimy - (2 + 1 + composer_height(m_input_buffer) + 2) - 2
                            - hook_progress_height());
    int content_h = prefix[n];
    int max_scroll = std::max(0, content_h - avail);

    // 确定视口顶行：跟随则钉底部，手动则钳制到 [0, max_scroll]
    int scroll_top;
    if (m_sub_follow) {
        scroll_top = max_scroll;
    } else {
        m_sub_scroll = std::max(0, std::min(m_sub_scroll, max_scroll));
        scroll_top = m_sub_scroll;
        if (m_sub_scroll >= max_scroll) {
            // 已滚到底部：恢复自动跟随（新步骤到来时继续钉在底部）
            m_sub_follow = true;
        }
    }

    // 只装配可视切片：顶/底用定高留白补齐，保持总高度与滚动范围不变
    m_sub_hits.clear();
    const int margin = std::max(8, avail / 2);
    TranscriptSlice slice = select_transcript_slice(prefix, scroll_top, avail, margin);

    Element content;
    if (slice.empty) {
        // 空记录或卷过底部：整段留白，保持内容高度以维持滚动范围
        content = pad_rows(content_h);
    } else {
        Elements es;
        if (slice.top_pad > 0) es.push_back(pad_rows(slice.top_pad));
        for (std::size_t i = slice.first; i <= slice.last; ++i) {
            const auto& m = vmsgs[i];
            const std::size_t hit_start = m_sub_hits.size();

            ftxui::Element msg_el;
            const std::deque<CardHit>* cached_hits = nullptr;
            if (m.sealed) {
                // sealed：按指纹复用渲染树，跳过 Markdown 解析/语法高亮
                const auto key = sealed_cache_key(m, width);
                auto& c = m_sub_cache[i];
                if (c.has && c.width == width && c.key == key) {
                    msg_el = c.element;
                    cached_hits = c.hits.get();
                } else {
                    if (!c.hits) c.hits = std::make_unique<std::deque<CardHit>>();
                    c.hits->clear();
                    c.element = build_message(m, width, 0, c.hits.get());
                    c.width = width;
                    c.key = key;
                    c.has = true;
                    msg_el = c.element;
                    cached_hits = c.hits.get();
                }
            } else {
                // 流式未封口：逐帧重建（命中反射当前帧坐标）
                msg_el = build_message(m, width, m_anim_frame, &m_sub_hits);
            }

            if (!msg_el) msg_el = ftxui::emptyElement();
            es.push_back(ftxui::hbox({
                ftxui::text("  "),
                ftxui::flex(std::move(msg_el)),
            }));

            if (cached_hits) {
                // 缓存卡片的 box 由 reflect 于渲染期回写，滚动时坐标随之刷新
                for (const auto& h : *cached_hits) {
                    CardHit c = h;
                    c.msg_idx = static_cast<int>(i);
                    m_sub_hits.push_back(c);
                }
            } else {
                for (std::size_t k = hit_start; k < m_sub_hits.size(); ++k)
                    m_sub_hits[k].msg_idx = static_cast<int>(i);
            }
            // 消息间空一行；与 prefix 的「每条 +1 间距」对齐
            es.push_back(ftxui::text(" "));
        }
        if (slice.bottom_pad > 0) es.push_back(pad_rows(slice.bottom_pad));
        content = ftxui::vbox(std::move(es));
    }

    // 回填卡片命中的步骤下标（思考卡/最终答复 sub_step=-1；工具卡映射到 steps 下标）
    for (std::size_t k = 0; k < m_sub_hits.size(); ++k) {
        auto& hit = m_sub_hits[k];
        if (hit.msg_idx >= 0 && static_cast<std::size_t>(hit.msg_idx) < vmap.size()) {
            hit.sub_step = vmap[static_cast<std::size_t>(hit.msg_idx)];
        }
    }

    // 状态头（固定，不参与虚拟化滚动）
    std::string status_icon = rec.status == "running" ? "●" : (rec.status == "failed" ? "✗" : "✓");
    Color status_color = rec.status == "running" ? theme::T::Accent
                        : (rec.status == "failed" ? Color::RedLight : Color::GreenLight);
    Element header = ftxui::hbox({
        ftxui::text("  "),
        ftxui::text(status_icon) | ftxui::color(status_color),
        ftxui::text(" "),
        ftxui::color(theme::T::Text)(ftxui::text(std::string(str::kSubHeader))),
        ftxui::text(" "),
        ftxui::text(truncate_task_id(rec.task_id)) | ftxui::color(theme::T::TextFaint),
        ftxui::text("  "),
        ftxui::color(theme::T::TextDim)(ftxui::text(status_text(rec.status))),
    });

    Element scroll;
    if (m_sub_follow) {
        // 跟随：钉在实际内容底部（focusPositionRelative 无估算误差）
        m_sub_scroll = max_scroll;  // 维护估计底部，切到手动滚动时平滑衔接
        scroll = content | ftxui::focusPositionRelative(0, 1.0f) | ftxui::yframe;
    } else {
        // 手动：+avail/2 抵消 frame 的居中，使 m_sub_scroll 即视口顶行（增大=向下=更新）
        scroll = content
            | ftxui::focusPosition(0, m_sub_scroll + avail / 2)
            | ftxui::yframe;
    }

    return ftxui::vbox({
        header,
        ftxui::text(" "),
        scroll | ftxui::yflex,
    });
}

/// @brief 切换到第二层并查看指定子 Agent 记录
void App::show_sub_agent(const std::string& task_id) {
    for (std::size_t i = 0; i < m_vm.sub_records.size(); ++i) {
        if (m_vm.sub_records[i].task_id == task_id) {
            m_vm.sub_active = static_cast<int>(i);
            m_vm.output_level = OutputLevel::SubAgent;
            m_sub_follow = true;
            m_sub_scroll = 0;
            m_screen.RequestAnimationFrame();
            return;
        }
    }
    // 找不到（记录尚未到达）：选最近一条
    if (!m_vm.sub_records.empty()) {
        m_vm.sub_active = static_cast<int>(m_vm.sub_records.size()) - 1;
        m_vm.output_level = OutputLevel::SubAgent;
        m_sub_follow = true;
        m_sub_scroll = 0;
    }
    m_screen.RequestAnimationFrame();
}

/// @brief 返回主会话层级
void App::show_main_level() {
    if (m_vm.output_level == OutputLevel::Main) return;
    m_vm.output_level = OutputLevel::Main;
    m_vm.sub_active = -1;
    m_screen.RequestAnimationFrame();
}

Element App::build_ask_modal() const {
    if (!m_ask_active) return ftxui::emptyElement();
    if (m_ask_qindex >= m_ask_questions.size()) return ftxui::emptyElement();

    const AskQuestion& q = m_ask_questions[m_ask_qindex];
    const std::string title =
        q.question.empty() ? std::string(str::kAskTitleFallback) : q.question;
    const std::string progress = std::string(str::kAskProgressPrefix) +
                                 std::to_string(m_ask_qindex + 1) +
                                 std::string(str::kAskProgressSep) +
                                 std::to_string(m_ask_questions.size());

    Elements body;
    body.push_back(ftxui::hbox({
        ftxui::text(std::string(str::kAskIcon)),
        ftxui::text(progress) | ftxui::color(theme::T::TextFaint),
        ftxui::separatorEmpty(),
        ftxui::flex(ftxui::paragraph(title) | ftxui::bold),
    }));
    body.push_back(ftxui::separatorEmpty());

    if (m_ask_custom) {
        // 自定义输入模式
        body.push_back(ftxui::hbox({ftxui::text(std::string(str::kAskCustomHint))
                                        | ftxui::color(theme::T::Text)}));
        body.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(m_ask_input->Render()),
        }));
    } else {
        // 选项列表
        for (size_t i = 0; i < q.options.size(); ++i) {
            const bool sel = (static_cast<int>(i) == m_ask_sel);
            std::string marker;
            if (q.multi) {
                marker = (i < m_ask_checked.size() && m_ask_checked[i])
                             ? std::string(str::kAskChecked)
                             : std::string(str::kAskUnchecked);
            } else {
                marker = sel ? std::string(str::kAskCursor) : "  ";
            }
            auto row = ftxui::hbox({
                ftxui::text("  " + marker),
                ftxui::flex(ftxui::text(q.options[i]) | ftxui::color(theme::T::Text)),
            });
            if (sel) row = row | ftxui::bgcolor(theme::T::Selection);
            body.push_back(row);
        }
        if (q.allow_custom_input) {
            const bool sel = (static_cast<int>(q.options.size()) == m_ask_sel);
            auto row = ftxui::hbox({
                ftxui::text(sel ? "  ❯ " : "    "),
                ftxui::flex(ftxui::text(std::string(str::kAskCustomOption))
                                | ftxui::color(theme::T::TextDim)),
            });
            if (sel) row = row | ftxui::bgcolor(theme::T::Selection);
            body.push_back(row);
        }
        body.push_back(ftxui::separatorEmpty());
        body.push_back(ftxui::hbox({
            ftxui::text(q.multi ? std::string(str::kAskMultiHint)
                                : std::string(str::kAskSingleHint))
                | ftxui::color(theme::T::TextFaint),
        }));
    }

    // 背景与输入框一致（Panel），避免黑底弹窗与灰底界面割裂
    return ftxui::xflex(ftxui::border(ftxui::vbox(std::move(body))))
        | ftxui::bgcolor(theme::T::Panel);
}

/// @brief 消息队列卡片（模型忙碌时前端入队的用户消息；输入框上方可折叠条）
/// @details 折叠态单行摘要（条数 + Ctrl+Enter 提示）；展开态逐条预览 + ✕ 移除。
///          命中区写入 m_queue_hits：标题行（button=-1）切换展开/折叠，
///          每条 ✕（button=条目下标）调用 ChatSession::remove_queued_message。
Element App::build_queue_bar() {
    m_queue_hits.clear();
    const auto& items = m_vm.message_queue.items;
    if (items.empty()) return ftxui::emptyElement();

    const bool expanded = m_vm.message_queue.expanded;

    // 标题行：图标 + 条数 + Ctrl+Enter 提示 + 展开/收起箭头；整行点击切换展开
    m_queue_hits.push_back(CardHit{});
    CardHit& title_hit = m_queue_hits.back();
    title_hit.msg_idx = -1;
    title_hit.tool_idx = -1;
    title_hit.button = -1;  // 标题行（切换展开/折叠）
    const std::string title = std::string(str::kQueueIcon) +
                              std::to_string(items.size()) +
                              std::string(str::kQueueTitlePrefix);
    auto title_body = ftxui::hbox({
        ftxui::text(title) | ftxui::color(theme::T::Text),
        ftxui::text(std::string(" · ")) | ftxui::color(theme::T::TextFaint),
        ftxui::text(std::string(str::kQueueCtrlHint))
            | ftxui::color(theme::T::TextFaint),
        ftxui::flex(ftxui::text("")),
        ftxui::text(std::string(expanded ? theme::icon_chevron_down()
                                         : theme::icon_chevron_right()))
            | ftxui::color(theme::T::TextFaint),
    }) | ftxui::reflect(title_hit.box);

    Elements rows;
    rows.push_back(ftxui::hbox({
        ftxui::text("  "),
        title_body | ftxui::flex,
        ftxui::text("  "),
    }) | ftxui::bgcolor(theme::T::Panel));

    if (expanded) {
        // 展开态：逐条预览 + 每条末尾 ✕ 移除按钮
        for (std::size_t i = 0; i < items.size(); ++i) {
            m_queue_hits.push_back(CardHit{});
            CardHit& hit = m_queue_hits.back();
            hit.msg_idx = static_cast<int>(i);
            hit.tool_idx = -1;
            hit.button = static_cast<int>(i);  // ✕ 移除该条

            // 预览：取首行单行展示（超宽由 text 裁剪，不换行）
            std::string line = items[i].text;
            const auto nl = line.find('\n');
            if (nl != std::string::npos) line = line.substr(0, nl);
            const std::string prefix = "  " + std::to_string(i + 1) + ". ";
            rows.push_back(ftxui::hbox({
                ftxui::text(prefix) | ftxui::color(theme::T::TextDim),
                ftxui::text(line) | ftxui::color(theme::T::Text),
                ftxui::flex(ftxui::text("")),
                ftxui::text(std::string(str::kQueueRemove))
                    | ftxui::color(theme::T::TextFaint)
                    | ftxui::reflect(hit.box),
                ftxui::text("  "),
            }) | ftxui::bgcolor(theme::T::Panel));
        }
    }

    return ftxui::vbox(std::move(rows));
}

// ---------------------------------------------------------------------------
// Hook 执行进度条（#50 M-2）
// ---------------------------------------------------------------------------

int App::hook_progress_height() const {
    return static_cast<int>(m_vm.hook_progress.size());
}

ftxui::Element App::build_hook_progress_elem() const {
    const auto& rows = m_vm.hook_progress;
    if (rows.empty()) return ftxui::emptyElement();

    Elements elems;
    elems.reserve(rows.size());
    for (const auto& r : rows) {
        // 状态图标 + 颜色（进行中蓝 / 完成绿 / 失败红）
        const bool running = r.phase == "start";
        const bool failed = r.phase == "failed";
        const std::string icon = failed ? "✕ " : (running ? "● " : "✓ ");
        const ftxui::Color icon_color =
            failed ? theme::T::DiffDel
                   : (running ? theme::T::Accent : theme::T::DiffAdd);

        std::string text = r.event;
        if (!r.tool_name.empty()) text += " · " + r.tool_name;
        if (!r.label.empty()) text += "  " + r.label;
        if (failed && !r.message.empty()) {
            std::string msg = r.message;
            const auto nl = msg.find('\n');
            if (nl != std::string::npos) msg = msg.substr(0, nl);
            if (msg.size() > 48) msg = msg.substr(0, 48) + "…";
            text += "  " + msg;
        }

        elems.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(icon) | ftxui::color(icon_color),
            ftxui::text(text) | ftxui::color(running ? theme::T::Text : theme::T::TextDim),
            ftxui::flex(ftxui::text("")),
            ftxui::text("  "),
        }) | ftxui::bgcolor(theme::T::Panel));
    }
    return ftxui::vbox(std::move(elems));
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

void App::run() {
    // 启动即写运行日志：确保每次运行都创建 ~/.workx/logs/workx_tui.log，
    // 而非仅在异常/mock 时才落盘（否则正常运行看不到该文件）
    log_run("app start");
#if defined(_WIN32)
    // stdout 为管道（如 opencode 终端）时 FTXUI 尺寸检测回退 80x24，
    // 用 CONOUT$ 兜底校准，避免渲染宽度不足导致右侧露白。
    int w = 0, h = 0;
    if (detect_console_size(w, h)) {
        ftxui::Terminal::SetFallbackSize({w, h});
    }
#endif
    // 冒烟模式（B5）：依赖 mock 流；自动驱动一轮对话后退出
    if (m_deps.smoke_mode) {
        m_deps.mock_mode = true;
        start_smoke_driver();
    }
    // 状态行：直接构建 Element（装饰性，非聚焦组件）
    auto build_status_elem = [this] {
        // #24：待办进度位（✓ X/Y）
        int todo_done = 0, todo_total = 0;
        for (const auto& t : m_vm.sidebar.todos) {
            ++todo_total;
            if (t.status == core::todo::TodoStatus::Completed) ++todo_done;
        }
        ftxui::Element line = build_status_line(m_vm.sidebar.model,
                                                m_vm.sidebar.mode,
                                                m_vm.sidebar.permission,
                                                m_vm.busy,
                                                m_anim_frame,
                                                todo_done, todo_total);
        // Ctrl+C 提示：单次按下后 1 秒内显示「再次按 Ctrl+C 退出」，超时自动隐藏
        if (m_ctrl_c_hint) {
            if (std::chrono::steady_clock::now() >= m_ctrl_c_hint_until) {
                m_ctrl_c_hint = false;
            } else {
                line = ftxui::hbox({
                    std::move(line),
                    ftxui::text("  "),
                    ftxui::text(std::string(str::kStatusCtrlC))
                        | ftxui::color(theme::T::Accent),
                });
            }
        }
        // Ctrl+G 异步编辑（Windows 记事本）进行中：状态行右侧常驻「编辑中」提示
        if (m_prompt_editing.load()) {
            line = ftxui::hbox({
                std::move(line),
                ftxui::text("  "),
                ftxui::text(std::string("✎ Prompt 编辑中 · Esc 完成"))
                    | ftxui::color(theme::T::Accent),
            });
        }
        return line;
    };

    // 命令条目：统一从 agent 命令注册表派生（B2 单一定义，无第二套注册表）
    // 供输入栏提示面板（/ 命令）与聚合搜索面板（功能类）共用
    m_palette_cmds.clear();
    if (m_deps.command_registry) {
        for (const auto& c : m_deps.command_registry->get_user_invocable_commands()) {
            m_palette_cmds.push_back(PaletteCommand{
                .command = "/" + c->name(),
                .title = c->name(),
                .description = c->description(),
                .keywords = c->description(),
            });
        }
    }

    ComposerOptions comp_opt;
    comp_opt.buffer = &m_input_buffer;
    comp_opt.cursor = &m_composer_cursor;
    comp_opt.on_submit = [this](const std::string& t) { send_input(t); };
    // Ctrl+Enter：模型忙碌时入队并请求下个工具轮边界立即冲刷
    comp_opt.on_submit_ctrl = [this](const std::string& t) { send_input(t, true); };
    comp_opt.on_perm_toggle = [this] { toggle_permission(); };
    comp_opt.on_mode_toggle = [this] { toggle_mode(); };
    comp_opt.on_toggle_thinking = [this] {
        if (!m_vm.messages.empty()) {
            auto& m = m_vm.messages.back();
            if (m.reasoned) {
                m.reasoning_expanded = !m.reasoning_expanded;
                m_screen.RequestAnimationFrame();
            }
        }
    };
    comp_opt.on_edit = [this] { edit_prompt(); };
    // 输入栏提示面板（/ 命令 · @ 文件）：状态机回调
    comp_opt.suggest_active = [this] { return m_suggest_mode != SuggestMode::None; };
    comp_opt.suggest_move = [this](int delta) { suggest_move(delta); };
    comp_opt.suggest_enter = [this] { return suggest_enter(); };
    comp_opt.suggest_enter_insert = [this] { return suggest_enter_insert(); };
    comp_opt.suggest_cancel = [this] { suggest_cancel(); };
    comp_opt.suggest_refresh = [this] { update_suggest(); };
    // 输入历史：上下箭头在首/末行时浏览历史（替换输入缓冲）
    comp_opt.on_history_prev = [this] {
        std::string out;
        if (!m_input_history.prev(m_input_buffer, out)) return false;
        m_input_buffer = out;
        m_composer_cursor = m_input_buffer.size();
        return true;
    };
    comp_opt.on_history_next = [this] {
        std::string out;
        if (!m_input_history.next(out)) return false;
        m_input_buffer = out;
        m_composer_cursor = m_input_buffer.size();
        return true;
    };

    m_composer = make_composer(comp_opt);

    // AskUser 模态输入（始终存在于组件树，按 m_ask_active 显隐）
    m_ask_input = ftxui::Input(&m_ask_buffer, std::string(str::kAskInputPlaceholder));

    // 全局聚合搜索面板（Ctrl+P）：entries 由 App 打开前装配、加载完成后更新，
    // 组件每帧重新过滤（选择位置保留）
    m_palette_comp = make_search_palette(
        m_search_entries,
        [this](int idx) {
            m_palette_open = false;
            apply_search_entry(idx);
            // 选中动作可能已打开新面板（模型/模式/供应商），焦点归新面板；
            // 仅无面板打开时恢复输入栏焦点
            if (!m_model_open && !m_mode_open && !m_resume_open && !m_provider_open) {
                if (m_composer) m_composer->TakeFocus();
            }
        },
        m_palette_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        "", true);

    // /model 模型面板：与搜索面板同款（标题 + 输入框 + Tab 循环；active = 当前模型）
    m_model_comp = make_search_palette(
        m_model_entries,
        [this](int idx) {
            m_model_open = false;
            apply_model(idx);
            if (m_composer) m_composer->TakeFocus();
        },
        m_model_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        std::string(str::kPaletteModelTitle));

    // 模式选择面板：与 /model 同款（标题 + 输入框；active = 当前模式，副标题 = 模式介绍）
    m_mode_comp = make_search_palette(
        m_mode_entries,
        [this](int idx) {
            m_mode_open = false;
            apply_mode(idx);
            if (m_composer) m_composer->TakeFocus();
        },
        m_mode_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        std::string(str::kPaletteModeTitle));

    // /resume 会话面板：仅会话条目（复用 m_session_metas 缓存）
    m_resume_comp = make_search_palette(
        m_session_entries,
        [this](int idx) {
            m_resume_open = false;
            if (idx >= 0 && idx < static_cast<int>(m_session_metas.size())) {
                const auto& s = m_session_metas[static_cast<size_t>(idx)];
                resume_session(s.file_path, s.title);
            }
            if (m_composer) m_composer->TakeFocus();
        },
        m_resume_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        std::string(str::kPaletteResumeTitle));

    // /provider 供应商管理：列表层可切换/编辑/添加/删除，表单层字段编辑
    // 列表改动经 on_commit 持久化；设为使用中走 switch_provider 热切换
    m_provider_comp = make_provider_manager(
        ftxtui::ProviderManagerOptions{
            .providers = m_providers,
            .active_id = m_current_provider,
            .catalog = m_deps.model_catalog ? m_deps.model_catalog->load() : nullptr,
            .on_activate = [this](int idx) {
                switch_provider(idx);
                if (m_composer) m_composer->TakeFocus();
            },
            .on_commit = [this] {
                if (m_deps.config_manager)
                    agent::save_provider_configs(*m_deps.config_manager, m_providers);
                // save_provider_configs 仅写内存；落盘，否则重启丢失新增/编辑的供应商条目
                if (m_deps.save_config) m_deps.save_config();
            },
            .on_close = [this] { if (m_composer) m_composer->TakeFocus(); },
            .title = std::string(str::kPaletteProviderTitle),
        },
        m_provider_open);

    // 子 Agent 菜单（任务调度 tab）：纵向可聚焦；Enter 跳转转录区对应消息
    {
        ftxui::MenuOption sub_opt = ftxui::MenuOption::Vertical();
        sub_opt.on_enter = [this] { jump_to_sub_agent(); };
        sub_opt.entries_option.transform = [](const ftxui::EntryState& s) {
            std::string prefix = s.active ? std::string(str::kAskCursor) : "  ";
            auto el = ftxui::text(prefix + s.label) | ftxui::color(theme::T::TextDim);
            if (s.active) el = el | ftxui::color(theme::T::Text);
            if (s.focused) el = el | ftxui::bold;
            return el;
        };
        m_sub_menu = ftxui::Menu(&m_sub_entries, &m_vm.tabs.sub_selected, sub_opt);
    }

    // 变更记录组件（变更记录 tab）：修改点 Menu + hunk + 目的展开；Enter 跳转文件 tab
    m_change_viewer = make_change_viewer(&m_vm.tabs.changes,
                                         [this] { jump_change_to_file(); });

    // 文件查看组件（文件 tab）：可聚焦，↑↓/PgUp/PgDn/滚轮滚动
    m_file_viewer = make_file_viewer(&m_vm.tabs.file);

    // 项目文件树组件（项目 tab，常驻）：点击目录展开/收起、点击文件打开、滚轮滚动
    m_project_tree = make_project_tree(&m_vm.tabs.project,
                                       [this](const std::string& rel) { open_project_file(rel); });

    // 可聚焦组件栈：composer、AskUser 输入、命令面板、模型/会话/供应商面板、子 Agent 菜单、变更记录、文件查看、项目文件树
    auto container = ftxui::Container::Vertical({
        m_composer,
        m_ask_input,
        m_palette_comp,
        m_model_comp,
        m_resume_comp,
        m_provider_comp,
        m_sub_menu,
        m_change_viewer,
        m_file_viewer,
        m_project_tree,
    });

    auto layout = ftxui::Renderer(container, [&]() -> ftxui::Element {
        try {
        ++m_anim_frame;  // 推进思考动画帧
#if defined(_WIN32)
        // 无 tty（stdout 为管道）时 FTXUI 无法感知终端 resize：
        // 每帧用 CONOUT$ 校准 fallback 尺寸，检测变化后当帧生效
        // （App::Draw 在 component->Render() 之后才取 Terminal::Size()）。
        {
            int w = 0, h = 0;
            if (detect_console_size(w, h)) {
                auto cur = ftxui::Terminal::Size();
                if (cur.dimx != w || cur.dimy != h) {
                    ftxui::Terminal::SetFallbackSize({w, h});
                }
            }
        }
#endif
        int width = ftxui::Terminal::Size().dimx;

        // 侧边栏宽度 = 终端宽度 × m_sidebar_width%（m_sidebar_width 现为百分比值）
        const bool show_sidebar_body = width >= kSidebarCollapseWidth;
        const int sidebar_cols = show_sidebar_body ? (width * m_sidebar_width / 100) : 0;
        const int sidebar_used = sidebar_cols > 0 ? (sidebar_cols + 1) : 0;
        const int content_w = std::max(24, width - sidebar_used);
        const int msg_width = std::max(1, content_w - 2);

        auto build_sidebar_elem = [this, sidebar_cols](const ftxui::Element& sub_menu_elem,
                                          const ftxui::Element& change_viewer_elem,
                                          const ftxui::Element& project_tree_elem,
                                          const ftxui::Element& file_viewer_elem) {
            // 镜像"项目 tab 可见"给后台扫描线程：仅可见时周期重扫，避免无谓扫描。
            m_project_tab_active.store(m_vm.tabs.active == SidebarTab::kProjects);
            return build_sidebar_tabs(m_vm.tabs, m_vm.sidebar, &m_tab_hits, &m_section_hits,
                                      sub_menu_elem, change_viewer_elem, project_tree_elem,
                                      file_viewer_elem)
                | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, sidebar_cols)
                | ftxui::yflex
                | ftxui::bgcolor(theme::T::Panel);
        };

        // 子 Agent 菜单条目重建 + 选中钳制（Menu 组件每帧渲染，box 供点击命中）
        m_sub_entries.clear();
        for (const auto& a : m_vm.tabs.sub_agents) m_sub_entries.push_back(sub_agent_label(a));
        if (m_vm.tabs.sub_selected < 0 ||
            m_vm.tabs.sub_selected >= static_cast<int>(m_sub_entries.size())) {
            m_vm.tabs.sub_selected = m_sub_entries.empty() ? -1 : 0;
        }
        Element sub_menu_elem = m_sub_entries.empty()
            ? ftxui::emptyElement()
            : m_sub_menu->Render() | ftxui::reflect(m_sub_box);

        // 变更记录组件元素（变更记录 tab 可交互；reflect 捕获 box 供点击命中）。
        // 非变更记录 tab 时置空 box，避免陈旧坐标误命中内容区点击。
        Element change_viewer_elem;
        if (m_vm.tabs.active == SidebarTab::kChanges && m_vm.tabs.changes_open) {
            change_viewer_elem = m_change_viewer->Render() | ftxui::reflect(m_change_box);
        } else {
            m_change_box = ftxui::Box{1, 0, 1, 0};
            change_viewer_elem = ftxui::emptyElement();
        }

        // 项目文件树组件（项目 tab，常驻）：Render 进布局并 reflect box，
        // 供 App 侧点击/滚轮命中转发到组件（目录展开/收起、文件打开、滚动）。
        Element project_tree_elem;
        if (m_vm.tabs.active == SidebarTab::kProjects) {
            project_tree_elem = m_project_tree->Render() | ftxui::reflect(m_project_box);
        } else {
            m_project_box = ftxui::Box{1, 0, 1, 0};
            project_tree_elem = ftxui::emptyElement();
        }

        // 文件查看器组件（文件 tab，可关）：Render 进布局并 reflect box，
        // 供 App 侧滚轮命中转发到组件（文件滚动）。返回 emptyElement 因组件
        // Render 已内联完整布局（含路径栏/分隔线/状态栏），勿二次包裹。
        Element file_viewer_elem;
        if (m_vm.tabs.active == SidebarTab::kFiles && m_vm.tabs.file_open &&
            !m_vm.tabs.file.path.empty()) {
            file_viewer_elem = m_file_viewer->Render() | ftxui::reflect(m_file_box);
        } else {
            m_file_box = ftxui::Box{1, 0, 1, 0};
            file_viewer_elem = ftxui::emptyElement();
        }

        // 后台任务：渲染时只读查询 TaskManager（原子字段，无锁安全）
        refresh_background_tasks();

        Element sidebar_elem = build_sidebar_elem(sub_menu_elem, change_viewer_elem,
                                                  project_tree_elem, file_viewer_elem);
        // 侧栏折叠时清空 tab / 区块 / 子 Agent 菜单命中区，避免陈旧 box 误命中内容区点击
        if (!show_sidebar_body) {
            m_tab_hits.clear();
            m_section_hits.clear();
            m_sub_box = ftxui::Box{1, 0, 1, 0};  // 空 box（IsEmpty=true），禁用点击命中
            m_change_box = ftxui::Box{1, 0, 1, 0};
            m_project_box = ftxui::Box{1, 0, 1, 0};
            m_file_box = ftxui::Box{1, 0, 1, 0};
        }
        // 输出区域按层级切换：主会话 → 转录区；子 Agent → 第二层独立记录渲染
        Element output_elem;
        if (m_vm.output_level == OutputLevel::SubAgent) {
            m_hits.clear();  // 第二层无卡片，清空旧坐标避免误命中
            output_elem = build_sub_agent_view(msg_width);
        } else {
            m_sub_hits.clear();  // 主层级无第二层卡片，清空旧坐标避免误命中
            output_elem = build_transcript(msg_width);
        }
        Element left_col = ftxui::flex(output_elem)
            | ftxui::bgcolor(theme::T::Surface);

        // 输入区：高度随内容行数增长（上限 5 行）+ 上/下/左内边距（灰底由底部面板统一提供）
        const int comp_h = composer_height(m_input_buffer);
        Element input_body = ftxui::vbox({
            ftxui::text(" "),                       // 顶部内边距（占一行）
            ftxui::hbox({                           // 左侧内边距
                ftxui::text("  "),
                ftxui::flex(m_composer->Render()),
            }),
            ftxui::text(" "),                       // 底部内边距（占一行）
        }) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, comp_h);

        // 状态行内嵌在输入区下方，与输入区共用灰底与左侧边框（左缩进 2 格）
        Element status_elem = ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(build_status_elem()),
        });

        // 底部面板：输入区 + 状态行 + 上下各一行空白（看起来内嵌一体）
        Element bottom_panel = ftxui::vbox({
            input_body,
            ftxui::text(" "),  // 状态行上方预留一行空白
            status_elem,
            ftxui::text(" "),  // 状态行下方预留一行空白（带背景色）
        }) | ftxui::bgcolor(theme::T::Panel);

        // 左侧高亮边框线贯穿整个底部面板（输入区 + 空白 + 状态行 + 空白）
        constexpr int kStatusHeight = 3;  // 状态行区域高度（含上下各 1 行空白）
        const int kPanelHeight = comp_h + kStatusHeight;
        Elements border_lines;
        for (int i = 0; i < kPanelHeight; ++i)
            border_lines.push_back(ftxui::text("│"));
        Element border = ftxui::vbox(std::move(border_lines))
            | ftxui::color(theme::T::Accent);

        Element composer_zone = ftxui::hbox({
            border,
            bottom_panel | ftxui::flex,
        });

        // 输入栏提示面板（/ 命令 · @ 文件）：紧贴输入区上方，激活时占位
        m_suggest_hits.clear();
        Element suggest_elem = render_suggest_panel(
            m_suggest_mode, m_suggest_entries, m_suggest_selected,
            agent::global_file_index().is_ready(), &m_suggest_hits);

        // 消息队列卡片（模型忙碌时前端入队的用户消息）：提示面板下方、输入区上方
        Element queue_elem = build_queue_bar();

        // Hook 执行进度条（#50 M-2）：输入区正上方，展示 Command/HTTP/Prompt hook 实时状态
        Element hook_elem = build_hook_progress_elem();

        // 左列：标题 + 层级子列表（面包屑导航）+ 转录 + 输入区（含内嵌状态行）
        Element content_col = ftxui::vbox({
            // 标题栏 = 面包屑（首项即当前会话标题），背景与输出区一致（Surface）
            build_breadcrumb(),
            left_col | ftxui::yflex,
            build_ask_modal(),
            suggest_elem,
            queue_elem,
            hook_elem,
            composer_zone,
        });

        // 侧边栏占满整列（顶到底），输入框右边即为侧边栏。
        // 按 m_sidebar_left 决定侧边栏居中位置（右 / 左）。
        Element content_col_el = content_col | ftxui::flex;
        Element side_space = show_sidebar_body ? ftxui::text(" ") : ftxui::emptyElement();
        Element side_el = show_sidebar_body ? sidebar_elem : ftxui::emptyElement();
        Element body = m_sidebar_left
                           ? ftxui::hbox({side_el, side_space, content_col_el})
                           : ftxui::hbox({content_col_el, side_space, side_el});

        // 面板以居中叠加方式呈现（不压缩内容区）
        // 命令面板：悬浮于主会话之上、不整屏清空背景（四周可见会话内容）
        Elements layers;
        layers.push_back(body);
        // 拖拽复制成功反馈：紧贴转录区底部、透明背景的一行浅字。
        // 超时由本帧（UI 线程）判定后隐藏，避免提示残留。
        if (m_copy_flash) {
            if (std::chrono::steady_clock::now() >= m_copy_flash_until) {
                m_copy_flash = false;
            } else {
                auto copy_msg = ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::text("已复制 " + std::to_string(m_copy_flash_n) + " 字符"),
                }) | ftxui::color(theme::T::Accent) | ftxui::clear_under;
                layers.push_back(ftxui::vbox({ftxui::filler(), copy_msg}));
            }
        }
        if (m_palette_open)
            layers.push_back(ftxui::center(m_palette_comp->Render()));
        if (m_model_open)
            layers.push_back(ftxui::center(m_model_comp->Render()));
        if (m_mode_open)
            layers.push_back(ftxui::center(m_mode_comp->Render()));
        if (m_resume_open)
            layers.push_back(ftxui::center(m_resume_comp->Render()));
        if (m_provider_open)
            layers.push_back(ftxui::center(m_provider_comp->Render()));
        // 整个背景使用黑色
        return ftxui::dbox(std::move(layers)) | ftxui::bgcolor(theme::T::Canvas);
        } catch (...) {
            // 防御：单帧渲染即便抛出（含非 std::exception 类型），也不得令整个程序退出。
            log_run("render exception: (unknown type)");
            return ftxui::text(std::string("渲染异常，重启以恢复正常"))
                   | ftxui::color(theme::T::Accent) | ftxui::bgcolor(theme::T::Canvas);
        }
    });

    auto root = layout | ftxui::CatchEvent([&](Event e) {
        if (e == Event::Custom) {
            drain();
            // Ctrl+G 异步编辑：消费轮询线程投递的最新 Prompt 内容，同步到输入框
            drain_prompt_pending();
            // Ctrl+G 异步编辑：检测到记事本关闭（真实进程退出）→ 自动收尾保存
            if (m_prompt_auto_done.exchange(false)) finish_prompt_editor();
            // 冒烟模式：UI 线程消费 driver 请求（投递消息 / 请求退出）
            if (m_smoke_submit.exchange(false)) {
                send_input("smoke: 渲染与滚动验证");
            }
            if (m_smoke_exit.exchange(false)) {
                m_screen.Exit();
            }
            // 思考动画期间持续推进重绘（动画线程定时 Post Custom）
            if (m_busy) m_screen.RequestAnimationFrame();
            return true;
        }
        if (m_ask_active) {
            // 防御：m_ask_active 与 m_ask_questions 不同步时（如解析后无有效问题），
            // 关闭模态并回填取消结果，避免 m_ask_questions[m_ask_qindex] 越界崩溃。
            if (m_ask_qindex >= m_ask_questions.size()) {
                close_ask(false);
                m_screen.RequestAnimationFrame();
                return true;
            }
            // B3：多问题 AskUser 交互
            // ↑↓ 移动选项；Enter 确认当前题/提交；空格（多选）勾选；
            // Esc 取消（自定义输入模式先返回选项）
            if (m_ask_custom) {
                if (e == Event::Return) { advance_ask(); m_screen.RequestAnimationFrame(); return true; }
                if (e == Event::Escape) { m_ask_custom = false; m_ask_sel = 0; m_ask_buffer.clear(); m_screen.RequestAnimationFrame(); return true; }
            } else {
                const AskQuestion& aq = m_ask_questions[m_ask_qindex];
                const int total = static_cast<int>(aq.options.size())
                                  + (aq.allow_custom_input ? 1 : 0);
                if (e == Event::ArrowUp) {
                    m_ask_sel = (m_ask_sel <= 0) ? total - 1 : m_ask_sel - 1;
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (e == Event::ArrowDown) {
                    m_ask_sel = (m_ask_sel >= total - 1) ? 0 : m_ask_sel + 1;
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (e == Event::Return) {
                    // 选中自定义输入项 → 进入输入模式；否则记录答案前进
                    if (aq.allow_custom_input &&
                        static_cast<size_t>(m_ask_sel) >= aq.options.size()) {
                        m_ask_custom = true;
                        m_ask_buffer.clear();
                        if (m_ask_input) m_ask_input->TakeFocus();
                    } else {
                        advance_ask();
                    }
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (aq.multi && e.character() == " ") {
                    if (m_ask_sel >= 0 && static_cast<size_t>(m_ask_sel) < m_ask_checked.size()) {
                        m_ask_checked[static_cast<size_t>(m_ask_sel)] =
                            !m_ask_checked[static_cast<size_t>(m_ask_sel)];
                    }
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (e == Event::Escape) { close_ask(false); m_screen.RequestAnimationFrame(); return true; }
            }
        }
        // Ctrl+P：全局聚合搜索面板（会话 / 文件 / 设置）。
        // 不放在输入栏消费：任意焦点下均可呼出（composer 已不拦截该键）
        {
            const std::string s(1, static_cast<char>(0x10));
            const bool ctrl_p = (e.is_character() && e.character() == s) ||
                                e == ftxui::Event::Special(s);
            if (ctrl_p) {
                // 面板互斥：同一时刻只开一个悬浮面板
                m_model_open = false;
                m_mode_open = false;
                m_resume_open = false;
                m_provider_open = false;
                // 打开前装配聚合条目（会话列表未加载则后台加载，面板刷新时自动出现）
                m_search_entries = assemble_search_entries();
                m_palette_open = true;
                if (m_palette_comp) m_palette_comp->TakeFocus();
                return true;
            }
        }
        // Ctrl+←/→：调整侧边栏宽度（Windows 补丁改写为 kitty 序列 \x1b[1;5D / \x1b[1;5C）。
        // 方向语义：箭头键始终推动侧边栏的外边缘——
        //   侧栏在右 → Ctrl+→ 收窄（向右推，侧栏占更少空间），Ctrl+← 增宽
        //   侧栏在左 → Ctrl+→ 增宽（向右推，侧栏占更多空间），Ctrl+← 收窄
        // 侧栏折叠（窄屏）时仍记录宽度，宽屏恢复后生效。
        if (e == ftxui::Event::Special("\x1b[1;5C")) {
            adjust_sidebar_width(m_sidebar_left ? +2 : -2);
            return true;
        }
        if (e == ftxui::Event::Special("\x1b[1;5D")) {
            adjust_sidebar_width(m_sidebar_left ? -2 : +2);
            return true;
        }
        // 悬浮面板打开时，把键盘事件手动路由给当前活动面板组件。
        // 面板组件（SearchPalette / ProviderManager）不在 FTXUI 标准焦点树中
        //（App 手动 Render，仅对鼠标手动路由 OnEvent），若不在此转发，
        // ↑↓ / Enter / 字符 / Backspace / Esc 等键盘事件都到不了面板，
        // 表现为「面板上下键没反应」也无法确认。鼠标事件仍走下方原路由。
        if ((m_palette_open || m_model_open || m_mode_open || m_resume_open ||
             m_provider_open) && !e.is_mouse()) {
            ftxui::Component active_panel = nullptr;
            if (m_palette_open) active_panel = m_palette_comp;
            else if (m_model_open) active_panel = m_model_comp;
            else if (m_mode_open) active_panel = m_mode_comp;
            else if (m_resume_open) active_panel = m_resume_comp;
            else if (m_provider_open) active_panel = m_provider_comp;
            if (active_panel && active_panel->OnEvent(e)) {
                m_screen.RequestAnimationFrame();
                return true;
            }
            // 面板未消费的 Esc：放行让面板自行处理（先清空搜索再关闭）。
            if (e == Event::Escape) return false;
        }
        // Esc：打断模型回复 / 工具调用（刷新 / AskUser / 面板的 Esc 已在上方各自处理）
        if (e == Event::Escape) {
            // Ctrl+G 异步编辑（Windows 记事本）进行中：Esc 结束编辑会话并同步输入框
            if (m_prompt_editing.load()) {
                finish_prompt_editor();
                return true;
            }
            // 输入栏"/"命令 / "@"文件提示面板激活时，Esc 优先关闭它。
            // 根 CatchEvent 先于 composer 收到 Esc（事件自上而下分发），
            // 若在此不放行，composer 的 Esc 分支永远跑不到，面板会关不掉。
            if (m_suggest_mode != SuggestMode::None) {
                suggest_cancel();
                if (m_composer) m_composer->TakeFocus();
                m_screen.RequestAnimationFrame();
                return true;
            }
            // 任一悬浮面板打开时，Esc 交回面板自身（先清空搜索再关闭），
            // 不在此拦截：否则根 CatchEvent 吞掉事件，面板永远收不到 Esc 而关不掉。
            if (m_palette_open || m_model_open || m_resume_open || m_provider_open)
                return false;
            // 第二层（子 Agent 记录）聚焦时，Esc 返回主会话层级
            if (m_vm.output_level == OutputLevel::SubAgent) {
                show_main_level();
                return true;
            }
            // 子 Agent 菜单聚焦时，Esc 退出菜单选择，焦点返回输入栏
            if (m_sub_menu && m_sub_menu->Focused()) {
                if (m_composer) m_composer->TakeFocus();
                m_screen.RequestAnimationFrame();
                return true;
            }
            // 侧边栏可开合 tab（变更记录/文件）激活时，Esc 先关闭该 tab（等价 ✕）
            if (m_vm.tabs.active == SidebarTab::kChanges && m_vm.tabs.changes_open) {
                close_sidebar_tab(SidebarTab::kChanges);
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (m_vm.tabs.active == SidebarTab::kFiles && m_vm.tabs.file_open) {
                close_sidebar_tab(SidebarTab::kFiles);
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (m_busy.load() && m_deps.event_bus)
                m_deps.event_bus->publish(agent::InterruptEvent{.force = false});
            return true;
        }
        // 文件 tab（/view 只读查看器）：↑↓/PgUp/PgDn 滚动（优先于转录区全局滚动）
        // 仅当 composer 未聚焦时拦截，否则 ↑↓ 应交给 composer 做历史回退/面板导航
        if (m_vm.tabs.active == SidebarTab::kFiles && m_vm.tabs.file_open &&
            !m_vm.tabs.file.path.empty() && !m_composer->Focused()) {
            const int total = static_cast<int>(m_vm.tabs.file.lines.size());
            const int visible = std::max(1, ftxui::Terminal::Size().dimy - 7);
            const int max_scroll = std::max(0, total - visible);
            int& sc = m_vm.tabs.file.scroll;
            if (e == Event::ArrowUp) {
                sc = std::max(0, sc - 1);
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (e == Event::ArrowDown) {
                sc = std::min(max_scroll, sc + 1);
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (e == Event::PageUp) {
                sc = std::max(0, sc - visible);
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (e == Event::PageDown) {
                sc = std::min(max_scroll, sc + visible);
                m_screen.RequestAnimationFrame();
                return true;
            }
        }
        // Ctrl+C：单次清空输入栏 + 状态栏提示；1 秒内连按两次 → 打断并退出
        {
            const std::string cs(1, static_cast<char>(0x03));
            const bool ctrl_c = (e.is_character() && e.character() == cs) ||
                                e == ftxui::Event::Special(cs);
            if (ctrl_c) {
                const auto now = std::chrono::steady_clock::now();
                if (m_last_ctrl_c != std::chrono::steady_clock::time_point{} &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_last_ctrl_c).count() <= 1000) {
                    m_last_ctrl_c = {};  // 双击：退出
                    m_ctrl_c_hint = false;
                    log_run("ctrl-c exit: double-press detected, calling on_exit");
                    if (m_deps.event_bus)
                        m_deps.event_bus->publish(agent::InterruptEvent{.force = true});
                    if (m_deps.on_exit) m_deps.on_exit();
                    else m_screen.Exit();
                    log_run("ctrl-c exit: on_exit returned");
                    return true;
                }
                m_last_ctrl_c = now;
                // 单次：仅清空输入栏 + 关闭提示面板（打断请用 Esc）
                m_input_buffer.clear();
                m_composer_cursor = 0;
                suggest_cancel();
                // 状态栏提示「再次按 Ctrl+C 退出」（1 秒窗口，渲染期超时自动隐藏）
                m_ctrl_c_hint = true;
                m_ctrl_c_hint_until = now + std::chrono::milliseconds(1000);
                m_screen.RequestAnimationFrame();
                return true;
            }
        }
        if (e.is_mouse()) {
            // 悬浮面板打开时，鼠标事件先转发给面板组件（点击选中 / 滚轮滚动列表）。
            // 面板未消费（如点击面板外）才落到下层转录的滚动/卡片/选中逻辑。
            ftxui::Component active_panel = nullptr;
            if (m_palette_open) active_panel = m_palette_comp;
            else if (m_model_open) active_panel = m_model_comp;
            else if (m_mode_open) active_panel = m_mode_comp;
            else if (m_resume_open) active_panel = m_resume_comp;
            else if (m_provider_open) active_panel = m_provider_comp;
            if (active_panel && active_panel->OnEvent(e)) {
                m_screen.RequestAnimationFrame();
                return true;
            }
            // 侧边栏拖动调整宽度：按住左键拖住内容区与侧栏之间的分隔线（"黑边"）
            // 即可改宽度。分隔线列 = 右栏 width-sidebar_cols-1 / 左栏 sidebar_cols，
            // 命中带宽 ±1 列便于抓取；拖动中以鼠标 x 精确换算目标列数后写回百分比。
            const int d_width = ftxui::Terminal::Size().dimx;
            const bool d_show_sidebar = d_width >= kSidebarCollapseWidth;
            if (d_show_sidebar) {
                const int d_bcols = d_width * m_sidebar_width / 100;
                const int d_border_col = m_sidebar_left ? d_bcols : (d_width - d_bcols - 1);
                const ftxui::Mouse& m = e.mouse();
                if (m.button == ftxui::Mouse::Left &&
                    m.motion == ftxui::Mouse::Pressed &&
                    m.x >= d_border_col - 1 && m.x <= d_border_col + 1) {
                    // 命中分隔线 → 开始拖动
                    m_sidebar_resizing = true;
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (m_sidebar_resizing) {
                    if (m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Moved) {
                        int target_cols = m_sidebar_left ? m.x : (d_width - 1 - m.x);
                        const int pct = target_cols * 100 / d_width;
                        constexpr int kMinW = 20, kMaxW = 80;
                        m_sidebar_width = std::clamp(pct, kMinW, kMaxW);
                        m_screen.RequestAnimationFrame();
                        return true;
                    }
                    if (m.motion == ftxui::Mouse::Released) {
                        m_sidebar_resizing = false;
                        m_screen.RequestAnimationFrame();
                        return true;
                    }
                }
            }
            if (e.mouse().button == ftxui::Mouse::WheelUp) {
                // 光标在文件查看器 box 内 → 转发滚动到文件组件并强制消费（不依赖组件返回值）
                if (!m_file_box.IsEmpty() && m_file_box.Contain(e.mouse().x, e.mouse().y) &&
                    m_file_viewer) {
                    m_file_viewer->OnEvent(e);
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                // 光标在项目文件树 box 内 → 滚动项目树而非主输出
                if (!m_project_box.IsEmpty() && m_project_box.Contain(e.mouse().x, e.mouse().y)) {
                    scroll_project(-3);
                    return true;
                }
                if (m_vm.output_level == OutputLevel::SubAgent) {
                    m_sub_follow = false;
                    m_sub_scroll = std::max(0, m_sub_scroll - 3);
                } else {
                    m_follow = false;
                    m_scroll = std::max(0, m_scroll - 3);
                }
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (e.mouse().button == ftxui::Mouse::WheelDown) {
                // 光标在文件查看器 box 内 → 转发滚动到文件组件并强制消费（不依赖组件返回值）
                if (!m_file_box.IsEmpty() && m_file_box.Contain(e.mouse().x, e.mouse().y) &&
                    m_file_viewer) {
                    m_file_viewer->OnEvent(e);
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                // 光标在项目文件树 box 内 → 滚动项目树而非主输出
                if (!m_project_box.IsEmpty() && m_project_box.Contain(e.mouse().x, e.mouse().y)) {
                    scroll_project(3);
                    return true;
                }
                if (m_vm.output_level == OutputLevel::SubAgent) {
                    m_sub_scroll += 3;
                } else {
                    m_scroll += 3;
                }
                m_screen.RequestAnimationFrame();
                return true;
            }
            // 点击折叠卡片：展开/收起思考或工具内容（直接用渲染 box 命中）
            if (e.mouse().button == ftxui::Mouse::Left &&
                e.mouse().motion == ftxui::Mouse::Pressed) {
                // 侧边栏 tab 栏：先检查 ✕ 关闭按钮，再检查 tab 切换
                if (!m_tab_hits.empty()) {
                    for (const auto& hit : m_tab_hits) {
                        if (hit.close && e.mouse().x >= hit.box.x_min &&
                            e.mouse().x <= hit.box.x_max &&
                            e.mouse().y >= hit.box.y_min &&
                            e.mouse().y <= hit.box.y_max) {
                            close_sidebar_tab(hit.tab);
                            m_screen.RequestAnimationFrame();
                            return true;
                        }
                    }
                    for (const auto& hit : m_tab_hits) {
                        if (!hit.close && e.mouse().x >= hit.box.x_min &&
                            e.mouse().x <= hit.box.x_max &&
                            e.mouse().y >= hit.box.y_min &&
                            e.mouse().y <= hit.box.y_max) {
                            m_vm.tabs.active = hit.tab;
                            // 切到变更记录 tab 时聚焦修改点列表（↑↓/e/Enter 立即可用）；
                            // 其余 tab 交还输入栏焦点
                            if (hit.tab == SidebarTab::kChanges && m_change_viewer)
                                m_change_viewer->TakeFocus();
                            else if (m_composer)
                                m_composer->TakeFocus();
                            m_screen.RequestAnimationFrame();
                            return true;
                        }
                    }
                }
                // 变更记录组件：点击转发（聚焦 + 选中该修改点 / 空白消费）
                if (!m_change_box.IsEmpty() &&
                    m_change_box.Contain(e.mouse().x, e.mouse().y)) {
                    m_change_viewer->OnEvent(e);
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                // 项目文件树组件：点击转发（目录展开/收起、文件打开）
                if (!m_project_box.IsEmpty() &&
                    m_project_box.Contain(e.mouse().x, e.mouse().y)) {
                    m_project_tree->OnEvent(e);
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                // 子 Agent 菜单：点击转发给 Menu（聚焦 + 选中该条目）
                if (!m_sub_entries.empty() && !m_sub_box.IsEmpty() &&
                    m_sub_box.Contain(e.mouse().x, e.mouse().y)) {
                    m_sub_menu->OnEvent(e);
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                // 侧栏可折叠区块（MCP/TODO）标题行：点击切换展开
                if (!m_section_hits.empty()) {
                    for (const auto& hit : m_section_hits) {
                        if (e.mouse().x >= hit.box.x_min &&
                            e.mouse().x <= hit.box.x_max &&
                            e.mouse().y >= hit.box.y_min &&
                            e.mouse().y <= hit.box.y_max) {
                            if (hit.kind == SectionHit::Kind::kMCP)
                                m_vm.sidebar.mcp_expanded = !m_vm.sidebar.mcp_expanded;
                            else
                                m_vm.sidebar.todo_expanded = !m_vm.sidebar.todo_expanded;
                            m_screen.RequestAnimationFrame();
                            return true;
                        }
                    }
                }
                // 提示面板（/ 命令 · @ 文件）候选行：点击选中并确认
                if (m_suggest_mode != SuggestMode::None && !m_suggest_hits.empty()) {
                    for (std::size_t i = 0; i < m_suggest_hits.size(); ++i) {
                        const auto& b = m_suggest_hits[i];
                        if (e.mouse().x >= b.x_min && e.mouse().x <= b.x_max &&
                            e.mouse().y >= b.y_min && e.mouse().y <= b.y_max) {
                            m_suggest_selected = static_cast<int>(i);
                            suggest_enter();
                            m_screen.RequestAnimationFrame();
                            return true;
                        }
                    }
                }
                // 消息队列卡片：标题行（button=-1）切换展开/折叠；✕（button=条目下标）移除该条
                if (!m_queue_hits.empty()) {
                    for (const auto& hit : m_queue_hits) {
                        if (e.mouse().x >= hit.box.x_min && e.mouse().x <= hit.box.x_max &&
                            e.mouse().y >= hit.box.y_min && e.mouse().y <= hit.box.y_max) {
                            if (hit.button >= 0) {
                                const auto& items = m_vm.message_queue.items;
                                if (hit.button < static_cast<int>(items.size()) &&
                                    m_deps.session) {
                                    m_deps.session->remove_queued_message(
                                        items[static_cast<std::size_t>(hit.button)].id);
                                }
                            } else {
                                m_vm.message_queue.expanded =
                                    !m_vm.message_queue.expanded;
                            }
                            m_screen.RequestAnimationFrame();
                            return true;
                        }
                    }
                }
                for (const auto& hit : m_breadcrumb_hits) {
                    if (e.mouse().x >= hit.box.x_min && e.mouse().x <= hit.box.x_max &&
                        e.mouse().y >= hit.box.y_min && e.mouse().y <= hit.box.y_max) {
                        // 主会话项 → 返回主层级；未到子 Agent 项 → 切换到第二层
                        if (hit.nav_target == 1) {
                            if (m_vm.output_level == OutputLevel::Main) {
                                m_vm.output_level = OutputLevel::SubAgent;
                                if (!m_vm.sub_records.empty())
                                    m_vm.sub_active = static_cast<int>(m_vm.sub_records.size()) - 1;
                                m_sub_follow = true;
                            }
                        } else {
                            show_main_level();
                        }
                        m_screen.RequestAnimationFrame();
                        return true;
                    }
                }
                // 第二层（子 Agent 记录）折叠卡片：点击展开/收起思考或工具内容
                for (const auto& hit : m_sub_hits) {
                    if (e.mouse().x >= hit.box.x_min && e.mouse().x <= hit.box.x_max &&
                        e.mouse().y >= hit.box.y_min && e.mouse().y <= hit.box.y_max) {
                        if (m_vm.sub_active < 0 ||
                            static_cast<std::size_t>(m_vm.sub_active) >= m_vm.sub_records.size()) {
                            break;
                        }
                        auto& rec = m_vm.sub_records[static_cast<std::size_t>(m_vm.sub_active)];
                        if (hit.button >= 0) {
                            // 消息操作按钮：复制最终答复可用；重试在第二层无意义，忽略
                            if (hit.button == 0 && !rec.final_answer.empty()
                                && write_clipboard(rec.final_answer)) {
                                flash_copy_message(utf8_char_count(rec.final_answer));
                            }
                        } else if (hit.sub_step < 0) {
                            rec.reasoning_expanded = !rec.reasoning_expanded;
                        } else if (static_cast<std::size_t>(hit.sub_step) < rec.steps.size()) {
                            rec.steps[static_cast<std::size_t>(hit.sub_step)].expanded =
                                !rec.steps[static_cast<std::size_t>(hit.sub_step)].expanded;
                        }
                        m_screen.RequestAnimationFrame();
                        return true;
                    }
                }
                for (const auto& hit : m_hits) {
                    if (e.mouse().x >= hit.box.x_min && e.mouse().x <= hit.box.x_max &&
                        e.mouse().y >= hit.box.y_min && e.mouse().y <= hit.box.y_max) {
                        auto& msg = m_vm.messages[static_cast<std::size_t>(hit.msg_idx)];
                        if (hit.button >= 0) {
                            // 消息操作按钮：复制 / 重试
                            if (hit.button == 0) {
                                if (!msg.text.empty() && write_clipboard(msg.text))
                                    flash_copy_message(utf8_char_count(msg.text));
                            } else if (hit.button == 1) {
                                retry_message(hit.msg_idx);
                            }
                        } else if (hit.tool_idx < 0) {
                            msg.reasoning_expanded = !msg.reasoning_expanded;
                        } else {
                            auto& t = msg.tool_calls[static_cast<std::size_t>(hit.tool_idx)];
                            // 子 Agent 工具卡：点击跳转到输出区域第二层（独立渲染），不展开
                            if (t.tool_name == "Agent") {
                                if (!t.sub_task_id.empty()) {
                                    show_sub_agent(t.sub_task_id);
                                } else {
                                    // 记录未到：切换到第二层看最近一条
                                    m_vm.output_level = OutputLevel::SubAgent;
                                    if (!m_vm.sub_records.empty()) {
                                        m_vm.sub_active = static_cast<int>(m_vm.sub_records.size()) - 1;
                                    } else {
                                        m_vm.sub_active = -1;
                                    }
                                    m_sub_follow = true;
                                    m_screen.RequestAnimationFrame();
                                }
                                return true;
                            }
                            t.expanded = !t.expanded;
                        }
                        m_screen.RequestAnimationFrame();
                        return true;
                    }
                }
            }
            // 拖拽选中完成（左键释放）：把最新选中文本写入系统剪贴板。
            // 若这次按下时点中了折叠卡片（上面已 return true → 选区被 FTXUI
            // 清空），m_selection_text 为空，跳过；否则正常复制。返回 false
            // 交给 FTXUI 保留选区高亮，供用户确认所复制的范围。
            if (e.mouse().button == ftxui::Mouse::Left &&
                e.mouse().motion == ftxui::Mouse::Released) {
                std::string sel = m_selection_text;
                while (!sel.empty() && (sel.back() == ' ' || sel.back() == '\t' ||
                                        sel.back() == '\n' || sel.back() == '\r'))
                    sel.pop_back();  // 选区末尾常带留白/换行，写入剪贴板前去净
                if (!sel.empty() && write_clipboard(sel))
                    flash_copy_message(utf8_char_count(sel));
            }
        }
        // 鼠标滚轮在管道终端（如 opencode）可能不转发事件，提供键盘替代：
        // Ctrl+↑/↓ 逐行滚动，PageUp/PageDown 整页滚动（按输出层级路由）
        const bool in_sub = (m_vm.output_level == OutputLevel::SubAgent);
        if (e == Event::ArrowUpCtrl) {
            if (in_sub) { m_sub_follow = false; m_sub_scroll = std::max(0, m_sub_scroll - 3); }
            else { m_follow = false; m_scroll = std::max(0, m_scroll - 3); }
            m_screen.RequestAnimationFrame();
            return true;
        }
        if (e == Event::ArrowDownCtrl) {
            if (in_sub) m_sub_scroll += 3;
            else m_scroll += 3;
            m_screen.RequestAnimationFrame();
            return true;
        }
        // 滚一页（转录区可视高度），PageUp 翻回上页、PageDown 翻回下页
        const int scroll_page =
            std::max(1, ftxui::Terminal::Size().dimy - (2 + 1 + composer_height(m_input_buffer) + 2));
        if (e == Event::PageDown) {
            if (in_sub) m_sub_scroll += scroll_page;
            else m_scroll += scroll_page;
            m_screen.RequestAnimationFrame();
            return true;
        }
        if (e == Event::PageUp) {
            if (in_sub) { m_sub_follow = false; m_sub_scroll = std::max(0, m_sub_scroll - scroll_page); }
            else { m_follow = false; m_scroll = std::max(0, m_scroll - scroll_page); }
            m_screen.RequestAnimationFrame();
            return true;
        }
        return false;
    });

    // 重绘驱动线程（A4：IDLE 零耗）：仅 busy（思考动画/流式）时按 80ms 节奏
    // 驱动帧；IDLE 时完全睡眠（等待 busy 变化唤醒），不再无条件 PostEvent 烧 CPU。
    // 无 tty 时的终端尺寸校准由每帧 Render 内 detect_console_size 完成。
    m_anim_run = true;
    m_anim_thread = std::thread([this] {
        std::unique_lock<std::mutex> lock(m_anim_mutex);
        while (m_anim_run) {
            if (!m_busy.load()) {
                // IDLE：阻塞等待（busy 置位或退出时唤醒）
                m_anim_cv.wait(lock, [this] { return !m_anim_run || m_busy.load(); });
                continue;
            }
            lock.unlock();
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            lock.lock();
        }
    });

    try {
        crash::ReinstallSignalHandlers();
        m_screen.Loop(root);
    } catch (const std::exception& ex) {
        log_run(std::string("loop exception: ") + ex.what());
        throw;
    }
}

// ---------------------------------------------------------------------------
// helper：权限模式标签
// ---------------------------------------------------------------------------

/// @brief 冒烟驱动（B5）：等待首帧后投递一条用户消息（走 mock 流），
///        轮询完成条件（busy 回落且已有 ≥2 条消息），再渲染约 1s 后请求退出。
///        15s 未完成按失败退出（m_exit_code=1）。driver 只经 atomic 与
///        UI 线程通信，不直接触碰 ViewModel。
void App::start_smoke_driver() {
    std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        m_smoke_submit.store(true);
        m_screen.PostEvent(Event::Custom);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!m_busy.load() && m_msg_count.load() >= 2) { done = true; break; }
        }
        // 完成后再渲染约 1s（思考动画/状态行/滚动路径均过一遍）
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        m_exit_code.store(done ? 0 : 1);
        m_smoke_exit.store(true);
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

std::string App::mode_label(agent::tool::PermissionMode m) {
    switch (m) {
        case agent::tool::PermissionMode::BypassPermissions: return "bypass";
        default: return "";
    }
}

/// @brief 权限两态切换（手动审批 ↔ 完全访问；Shift+Tab / 设置面板）
/// @details 真实模式：session 侧切换并回读；mock 模式：本地循环 "" → bypass
void App::toggle_permission() {
    if (m_deps.session) {
        m_deps.session->toggle_permission_mode();
        m_vm.sidebar.permission = mode_label(m_deps.session->permission_mode());
    } else {
        static const char* kCycle[] = {"", "bypass"};
        m_mock_perm_cycle = (m_mock_perm_cycle + 1) % 2;
        m_vm.sidebar.permission = kCycle[m_mock_perm_cycle];
    }
    m_vm.apply(ActionPermissions{.label = m_vm.sidebar.permission});
}

/// @brief 会话工作模式 → 状态行标签（"standard" / "plan" / "minimal"）
std::string App::session_mode_label(agent::tool::SessionMode m) {
    switch (m) {
        case agent::tool::SessionMode::Plan: return "plan";
        case agent::tool::SessionMode::Minimal: return "minimal";
        default: return "standard";
    }
}

/// @brief 工作模式三态切换（标准 → 极简 → 计划 → 标准；Tab / Ctrl+T / 设置面板）
/// @details 真实模式：session 侧切换（计划联动权限 Plan，退出恢复）并回读；
///          mock 模式：本地循环 standard → minimal → plan
void App::toggle_mode() {
    if (m_deps.session) {
        m_deps.session->toggle_session_mode();
        m_vm.sidebar.mode = session_mode_label(m_deps.session->session_mode());
    } else {
        static const char* kModeCycle[] = {"standard", "minimal", "plan"};
        m_mock_mode_cycle = (m_mock_mode_cycle + 1) % 3;
        m_vm.sidebar.mode = kModeCycle[m_mock_mode_cycle];
    }
    m_vm.apply(ActionSetMode{.label = m_vm.sidebar.mode});
}

// ---------------------------------------------------------------------------
// 拖拽选中 → 复制剪贴板（FTXUI 原生 Selection + 系统剪贴板）
// ---------------------------------------------------------------------------

/// @brief 选区文本变化回调：SelectionChange 由 FTXUI 在每次绘制后、且选区实际
///        变化时调用（主 loop 线程）。这里缓存最新选中文本，供鼠标释放时复制。
void App::on_selection_changed() {
    m_selection_text = m_screen.GetSelection();
}

/// @brief 显示「已复制 N 字符」短暂提示。自清除线程只 sleep 后触发一次全局重绘
///        （ftxui::animation::RequestAnimationFrame），不触碰任何 App 成员，
///        因此可安全 detach/join；超时判定由主线程在渲染时完成。
void App::flash_copy_message(std::size_t char_count) {
    m_copy_flash_n = char_count;
    m_copy_flash = true;
    m_copy_flash_until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(1500);
    if (m_copy_flash_thread.joinable()) m_copy_flash_thread.detach();
    m_copy_flash_thread = std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        ftxui::animation::RequestAnimationFrame();  // 全局触发重绘，清除提示
    });
}

/// @brief 重试指定助手消息：截断到其触发用户消息并重新生成
void App::retry_message(int msg_idx) {
    if (m_vm.busy) return;  // 生成中不重试
    // 找到该助手消息之前的最后一条用户消息（触发该回复的 prompt）
    int user_idx = -1;
    for (int i = msg_idx - 1; i >= 0; --i) {
        if (m_vm.messages[static_cast<std::size_t>(i)].role == MsgRole::User) {
            user_idx = i;
            break;
        }
    }
    if (user_idx < 0) return;
    const std::string user_text = m_vm.messages[static_cast<std::size_t>(user_idx)].text;
    // 截断 VM：删除该助手消息及其后所有消息（保留触发它的用户消息）
    m_vm.messages.erase(m_vm.messages.begin() + msg_idx, m_vm.messages.end());
    invalidate_msg_cache();
    m_follow = true;
    if (m_deps.mock_mode) {
        start_mock_stream(user_text);
        m_screen.PostEvent(Event::Custom);
        return;
    }
    if (!m_deps.session) return;
    // 会话侧：截断到该用户消息并重新生成
    m_deps.session->regenerate_from(user_text);
    m_vm.apply(ActionSetBusy{.busy = true});
    m_screen.PostEvent(Event::Custom);  // 唤醒事件循环消费积压事件
}

/// @brief 关闭侧边栏可开合 tab（变更记录/文件）
/// @details 关闭活动 tab 后回到常驻内容 tab：文件查看器关闭跳转「项目」文件树，
///          变更记录关闭跳转「任务调度」。
void App::close_sidebar_tab(SidebarTab tab) {
    if (tab == SidebarTab::kChanges) m_vm.tabs.changes_open = false;
    if (tab == SidebarTab::kFiles) m_vm.tabs.file_open = false;
    if (m_vm.tabs.active == tab) {
        m_vm.tabs.active =
            (tab == SidebarTab::kFiles) ? SidebarTab::kProjects : SidebarTab::kTasks;
        if (m_composer) m_composer->TakeFocus();  // 关闭后交还输入栏焦点
    }
}

/// @brief 调整侧边栏宽度百分比（Ctrl+← 减小 / Ctrl+→ 增大，钳制在 [20, 80]）
void App::adjust_sidebar_width(int delta) {
    constexpr int kMinSidebarWidth = 20;
    constexpr int kMaxSidebarWidth = 80;
    m_sidebar_width = std::clamp(m_sidebar_width + delta, kMinSidebarWidth, kMaxSidebarWidth);
    m_screen.RequestAnimationFrame();
}

/// @brief 跳转文件 tab 到选中修改点对应行（变更记录 tab Enter）
/// @details 目标文件未打开则经 cmd_view 读取；已打开则仅切 tab。
///          定位修改区块起始行（locate_block）并滚动到该行。
void App::jump_change_to_file() {
    const int idx = m_vm.tabs.changes.selected;
    if (idx < 0 || static_cast<std::size_t>(idx) >= m_vm.tabs.changes.changes.size())
        return;
    const auto& ch = m_vm.tabs.changes.changes[static_cast<std::size_t>(idx)];

    // 打开目标文件（未打开则读取；已打开同一文件则仅切 tab）
    cmd_view(ch.file_path);

    // 定位修改区块起始行并滚动（cmd_view 已切到文件 tab）
    namespace fs = std::filesystem;
    fs::path cp(ch.file_path);
    if (!cp.is_absolute()) cp = fs::current_path() / cp;
    std::error_code ec;
    const std::string canon = fs::weakly_canonical(cp, ec).string();
    // tabs.file.path 可能为相对路径（/view ./src/x.cpp），统一 canonical 后比较
    fs::path fp(m_vm.tabs.file.path);
    if (!fp.is_absolute()) fp = fs::current_path() / fp;
    std::error_code ec2;
    const std::string file_canon = fs::weakly_canonical(fp, ec2).string();
    if (m_vm.tabs.file_open && file_canon == canon) {
        const int start = locate_block(m_vm.tabs.file.lines, ch.new_string);
        if (start >= 0) m_vm.tabs.file.scroll = start;
    }
    if (m_composer) m_composer->TakeFocus();  // 文件 tab 为纯视图，交还输入栏焦点
    m_screen.RequestAnimationFrame();
}

/// @brief 跳转到输出区域第二层查看选中子 Agent 记录（任务调度 tab Menu Enter）
/// @details 子 Agent 记录独立渲染于第二层，选中后切到该子 Agent 详情。
void App::jump_to_sub_agent() {
    const int idx = m_vm.tabs.sub_selected;
    if (idx < 0 || static_cast<std::size_t>(idx) >= m_vm.tabs.sub_agents.size()) return;
    const std::string& task_id = m_vm.tabs.sub_agents[static_cast<std::size_t>(idx)].task_id;
    show_sub_agent(task_id);
}

/// @brief 刷新后台任务列表（渲染时只读查询 TaskManager，仅进行中/排队中）
void App::refresh_background_tasks() {
    m_vm.tabs.background_tasks.clear();
    if (!m_deps.task_manager) return;
    for (const auto& t : m_deps.task_manager->getTasks()) {
        const auto st = t->getStatus();
        if (st == agent::TaskStatus::Completed ||
            st == agent::TaskStatus::Cancelled ||
            st == agent::TaskStatus::Failed) {
            continue;
        }
        TaskLite lite;
        lite.name = t->getName();
        lite.progress = t->getProgressPercent();
        lite.status = st == agent::TaskStatus::Running ? "Running" : "Pending";
        m_vm.tabs.background_tasks.push_back(std::move(lite));
    }
}

// ---------------------------------------------------------------------------
// 项目文件树（项目 tab，常驻）：后台 git 扫描 + 树构建 + 打开/滚动
// ---------------------------------------------------------------------------
namespace {

/// @brief 运行 git 命令（cwd 下），返回 stdout 非空行（空格分隔参数，本场景无参数含空格）
std::vector<std::string> run_git_lines(const std::string& cwd, const char* args_joined) {
    agent::process::ExecOptions opts;
    opts.cwd = cwd;
    opts.timeout = std::chrono::milliseconds(15000);
    std::string cur;
    for (const char c : std::string(args_joined)) {
        if (c == ' ') {
            if (!cur.empty()) { opts.args.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) opts.args.push_back(cur);
    auto res = agent::process::exec("git", opts);
    if (!res.is_ok()) return {};
    std::vector<std::string> lines;
    std::string t;
    for (const char c : res.value().stdout_text) {
        if (c == '\n') {
            if (!t.empty()) lines.push_back(t);
            t.clear();
        } else {
            t += c;
        }
    }
    if (!t.empty()) lines.push_back(t);
    return lines;
}

/// @brief 向树根插入一条文件路径（按 '/' 分段；目录由父段隐式合成），并把 git 状态写回叶子
void add_node_path(std::vector<ProjectNode>& root, const std::string& rel,
                   const std::map<std::string, char>& status) {
    std::vector<std::string> comps;
    std::size_t pos = 0;
    while (pos <= rel.size()) {
        const auto idx = rel.find('/', pos);
        if (idx == std::string::npos) { comps.push_back(rel.substr(pos)); break; }
        comps.push_back(rel.substr(pos, idx - pos));
        pos = idx + 1;
    }
    std::vector<ProjectNode>* level = &root;
    std::string run;
    for (std::size_t i = 0; i < comps.size(); ++i) {
        const bool last = (i + 1 == comps.size());
        run = run.empty() ? comps[i] : run + "/" + comps[i];
        ProjectNode* slot = nullptr;
        for (auto& nd : *level)
            if (nd.name == comps[i] && nd.rel_path == run) { slot = &nd; break; }
        if (!slot) {
            level->push_back(ProjectNode{});
            slot = &level->back();
            slot->name = comps[i];
            slot->rel_path = run;
            slot->is_dir = !last;
        }
        if (last && !status.empty()) {
            const auto it = status.find(rel);
            if (it != status.end()) { slot->status = it->second; slot->has_status = (it->second != ' '); }
        }
        level = &slot->children;
    }
}

/// @brief 目录优先 + 名称（不区分大小写）排序，递归
void sort_project_tree(std::vector<ProjectNode>& nodes) {
    const auto lower = [](const std::string& s) {
        std::string r;
        r.reserve(s.size());
        for (const unsigned char c : s) r += static_cast<char>(std::tolower(c));
        return r;
    };
    std::stable_sort(nodes.begin(), nodes.end(),
                     [&](const ProjectNode& a, const ProjectNode& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;  // 目录优先
        const auto la = lower(a.name), lb = lower(b.name);
        if (la != lb) return la < lb;
        return a.name < b.name;
    });
    for (auto& nd : nodes)
        if (nd.is_dir) sort_project_tree(nd.children);
}

/// @brief 由文件路径集合 + git 状态表构造排序文件树
std::vector<ProjectNode> build_project_tree(const std::set<std::string>& paths,
                                            const std::map<std::string, char>& status) {
    std::vector<ProjectNode> root;
    for (const auto& p : paths) add_node_path(root, p, status);
    sort_project_tree(root);
    return root;
}

/// @brief 非 git 仓库时的文件系统遍历（跳过重型目录，限制条目数）
void walk_fs(const std::string& root, std::set<std::string>& out) {
    namespace fs = std::filesystem;
    static const std::set<std::string> kSkip = {
        ".git", ".svn", ".hg", "node_modules", "build", "dist", "target", "out",
        "__pycache__", ".venv", "venv", "cmake-build-debug", "cmake-build-release",
        ".idea", ".vscode",
    };
    const std::string slash = std::string(1, '/');
    std::error_code ec;
    fs::recursive_directory_iterator it(fs::path(root),
                                        fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    int guard = 0;
    try {
        for (; it != end; it.increment(ec)) {
            if (ec) break;
            if (it->is_directory(ec)) {
                if (kSkip.count(it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec)) continue;
            if (++guard > 20000) break;
            const std::string rel = fs::relative(it->path(), root, ec).generic_string();
            if (!rel.empty()) out.insert(rel);
        }
    } catch (...) {
    }
}

}  // namespace

/// @brief 后台扫描项目文件树 + git 状态并推送 UI（项目 tab）
/// @details 单个常驻线程：先首扫（含 loading 占位）；随后仅在项目 tab 可见时周期重扫，
///          自动反映磁盘文件变化。重扫不置 loading（避免每帧闪烁"扫描项目文件中…"），
///          apply_variant 内 merge_project_expand 保留既有目录展开态。
///          git 仓库：git ls-files（已跟踪）+ git status --porcelain（改动/未跟踪）取并集，
///          天然忽略被 ignore 的文件；非 git：文件系统遍历回退（无状态点）。
void App::start_project_scan() {
    if (m_project_watch_run.exchange(true)) return;
    const std::string root = std::filesystem::current_path().string();
    m_project_watch_thread = std::thread([this, root] {
        const auto scan = [root]() {
            ActionProjectFiles act;
            act.root = root;
            act.loading = false;
            const bool is_git =
                std::filesystem::is_directory(std::filesystem::path(root) / ".git");
            act.is_git = is_git;
            if (is_git) {
                std::set<std::string> paths;
                std::map<std::string, char> status;
                for (auto& line : run_git_lines(root, "ls-files")) {
                    if (line.empty()) continue;
                    paths.insert(line);
                    status[line] = ' ';
                }
                for (auto& line : run_git_lines(root, "status --porcelain --untracked-files=all")) {
                    if (line.size() < 3) continue;
                    const char x = line[0], y = line[1];
                    std::string p = line.substr(3);  // 跳 "XY " / "?? "
                    if (!p.empty() && p.front() == '"' && p.size() >= 2 && p.back() == '"')
                        p = p.substr(1, p.size() - 2);
                    const auto arrow = p.find(" -> ");  // 重命名/复制取新名
                    if (arrow != std::string::npos) p = p.substr(arrow + 4);
                    if (p.empty()) continue;
                    const char code = (x == '?' && y == '?') ? '?'
                                     : (y != ' ' ? y : x);
                    paths.insert(p);
                    status[p] = code;
                }
                act.tree = build_project_tree(paths, status);
            } else {
                std::set<std::string> paths;
                walk_fs(root, paths);
                act.tree = build_project_tree(paths, {});
            }
            return act;
        };

        // 首轮：loading 占位 + 完整扫描快照（PostEvent 唤醒事件循环消费并重绘）
        m_queue.push(ActionProjectFiles{.root = root, .loading = true});
        m_queue.push(scan());
        m_screen.PostEvent(Event::Custom);

        // 周期重扫：仅当项目 tab 可见，避免后台无谓拉起 git 进程
        constexpr auto kInterval = std::chrono::seconds(2);
        while (m_project_watch_run.load()) {
            std::this_thread::sleep_for(kInterval);
            if (!m_project_watch_run.load()) break;
            if (!m_project_tab_active.load()) continue;
            m_queue.push(scan());
            m_screen.PostEvent(Event::Custom);  // 唤醒 UI 消费并重绘，实现自动更新可见
        }
    });
}

/// @brief 点击项目文件行打开查看器（相对项目根 → /view）
void App::open_project_file(const std::string& rel_path) {
    std::string abs = rel_path;
    if (!m_vm.tabs.project.root.empty()) {
        std::error_code ec;
        const std::string joined =
            (std::filesystem::path(m_vm.tabs.project.root) / rel_path)
                .lexically_normal().string();
        const std::filesystem::path canon = std::filesystem::weakly_canonical(joined, ec);
        abs = ec ? joined : canon.string();
    }
    cmd_view(abs);
}

/// @brief 项目树方向键/滚轮滚动（钳制并请求重绘）
void App::scroll_project(int delta) {
    auto& p = m_vm.tabs.project;
    const int total = static_cast<int>(flatten_project_rows(p.tree).size());
    const int visible = std::max(1, ftxui::Terminal::Size().dimy - 7);
    const int max_scroll = std::max(0, total - visible);
    p.scroll = std::clamp(p.scroll + delta, 0, max_scroll);
    m_screen.RequestAnimationFrame();
}

}  // namespace ftxtui