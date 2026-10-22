#include "vm/view_model.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "theme/strings.h"

namespace ftxtui {

MessageNode& ViewModel::active_stream() {
    if (!messages.empty()) {
        auto& back = messages.back();
        if (back.role == MsgRole::Assistant && !back.sealed) {
            return back;
        }
    }
    messages.push_back(MessageNode{});
    return messages.back();
}

bool ViewModel::has_active_stream() const {
    if (messages.empty()) return false;
    const auto& back = messages.back();
    return back.role == MsgRole::Assistant && !back.sealed;
}

bool ViewModel::apply(const Action& action) {
    return std::visit([this](const auto& a) -> bool { return apply_variant(a); }, action);
}

// 分派单个 action（private helper，经由 std::visit 调用）
bool ViewModel::apply_variant(const ActionAppendMessage& a) {
    MessageNode n;
    n.role = a.role == "user" ? MsgRole::User : MsgRole::Assistant;
    n.text = a.text;
    n.sealed = true;
    // assistant 追加均为 UI 通知/命令输出，非模型回复 → 不显示重试/复制按钮栏
    n.notice = (n.role == MsgRole::Assistant);
    messages.push_back(std::move(n));
    return true;
}

bool ViewModel::apply_variant(const ActionAppendSkill& a) {
    MessageNode n;
    n.role = MsgRole::Assistant;
    n.sealed = true;
    ToolCallNode t;
    t.tool_name = "Skill";
    t.call_id = "";  // 本地合成，无真实 tool_call id
    // arguments 需为 {"name":...} JSON，skill_name_from_args 据此显示「Skills：名」
    t.arguments = nlohmann::json{{"name", a.name}}.dump();
    if (a.is_error) {
        t.result = "(技能本地解析失败)";
    } else {
        t.result = "(技能已本地解析，指令已交由模型处理" +
                   (a.input.empty() ? std::string{} : std::string("：") + a.input) + ")";
    }
    t.done = true;  // 本地技能同步解析完成
    t.running = false;
    t.is_error = a.is_error;
    t.expanded = a.is_error;  // 出错默认展开
    t.text_pos = n.text.size();
    n.tool_calls.push_back(std::move(t));
    messages.push_back(std::move(n));
    return true;
}

bool ViewModel::apply_variant(const ActionAppendCmdResult& a) {
    // ！命令执行结果：合成独立 assistant 消息承载 Bash 卡（复用 Shell 工具卡渲染：
    // tool_name="Bash" → 标题取 arguments.command，结果剥离 <stdout>/<stderr>/<exit_code>
    // 标签、<error> 红色显示）。本地合成完成态（无 running），出错默认展开。
    MessageNode n;
    n.role = MsgRole::Assistant;
    n.sealed = true;
    n.notice = true;  // UI 通知，非模型回复 → 不显示重试/复制按钮栏
    ToolCallNode t;
    t.tool_name = "Bash";
    t.call_id = "";  // 本地合成，无真实 tool_call id
    t.arguments = nlohmann::json{{"command", a.command}}.dump();
    t.result = a.result;
    t.done = true;
    t.running = false;
    t.is_error = a.is_error;
    t.expanded = a.is_error;
    t.text_pos = n.text.size();
    n.tool_calls.push_back(std::move(t));
    messages.push_back(std::move(n));
    return true;
}

bool ViewModel::apply_variant(const ActionSubmitCmdToModel&) {
    // 由 App 层在 drain 中处理（回显 user + 置 busy + on_submit），此处仅满足
    // std::visit 编译对全部 variant 类型需有重载的要求，不参与渲染。
    return false;
}

bool ViewModel::apply_variant(const ActionTokenDelta& a) {
    auto& m = active_stream();
    m.streaming = true;
    m.text.append(a.content_delta);
    return true;
}

bool ViewModel::apply_variant(const ActionReasoningDelta& a) {
    auto& m = active_stream();
    if (!a.delta.empty()) {
        if (!m.reasoned) {
            m.reasoned = true;
            m.reasoning_expanded = true;  // 任务开始：思考卡自动展开
        }
    }
    m.reasoning.append(a.delta);
    return true;
}

bool ViewModel::apply_variant(const ActionStepDone&) {
    // 单步结束但仍有工具待执行：不封口，仅停止流式标记
    if (!messages.empty() && messages.back().role == MsgRole::Assistant) {
        messages.back().streaming = false;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionTurnDone& a) {
    auto& m = active_stream();
    m.streaming = false;
    m.sealed = true;
    m.reasoning_expanded = false;  // 任务结束：思考卡自动折叠
    if (!a.full_content.empty() && m.text.empty()) m.text = a.full_content;
    if (!a.full_reasoning.empty() && m.reasoning.empty()) m.reasoning = a.full_reasoning;
    m.prompt_tokens = a.prompt_tokens;
    m.generated_tokens = a.generated_tokens;
    m.cache_read_tokens = a.cache_read_input_tokens;
    m.duration_ms = a.prompt_ms + a.generation_ms;
    m.reasoning_ms = a.reasoning_ms;
    total_tokens = a.prompt_tokens + a.generated_tokens + a.cache_read_input_tokens;
    sidebar.context_used = a.prompt_tokens;
    // #65：细粒度分项累计（保留 resume 后历史，避免覆盖丢失）
    sidebar.prompt_tokens += a.prompt_tokens;
    sidebar.generated_tokens += a.generated_tokens;
    sidebar.cache_read_tokens += a.cache_read_input_tokens;
    sidebar.cache_hit_tokens += a.prompt_cache_hit_tokens;
    sidebar.cache_miss_tokens += a.prompt_cache_miss_tokens;
    sidebar.total_tokens += total_tokens;
    busy = false;
    return true;
}

bool ViewModel::apply_variant(const ActionError& a) {
    MessageNode n;
    n.role = MsgRole::Error;
    n.text = a.message;
    n.sealed = true;
    messages.push_back(std::move(n));
    busy = false;
    return true;
}

bool ViewModel::apply_variant(const ActionBeginTool& a) {
    auto& m = active_stream();
    if (m.find_tool(a.call_id)) return false;  // 去重
    ToolCallNode t;
    t.tool_name = a.tool_name;
    t.call_id = a.call_id;
    t.arguments = a.arguments;
    t.running = true;
    t.expanded = true;           // 任务开始：工具卡自动展开
    t.text_pos = m.text.size();  // 记录正文插入点，供与正文交错渲染
    m.tool_calls.push_back(std::move(t));
    m.tool_use_ids.push_back(a.call_id);

    // P4 修改追踪：Edit/Write 工具调用 → FileChange（内联 diff 高亮数据源）
    if (a.tool_name == "Edit" || a.tool_name == "Write") {
        track_file_change(a);
    }
    return true;
}

bool ViewModel::apply_variant(const ActionEndTool& a) {
    // 在最后一条 assistant（可为已封口）里找；找不到则全表找
    for (size_t i = messages.size(); i-- > 0;) {
        auto& m = messages[i];
        if (auto* t = m.find_tool(a.call_id)) {
            t->result = a.result;
            t->done = true;
            t->running = false;
            t->is_error = a.is_error;
            t->expanded = a.is_error;  // 出错默认展开
            return true;
        }
    }
    return false;
}

bool ViewModel::apply_variant(const ActionAgentDone& a) {
    busy = false;
    // 成功路径 StreamDoneEvent（ActionTurnDone）已先行封口并填充 final_answer
    // （同一线程顺序发布，队列保序），AgentDoneEvent 只是最终汇总：
    // 仅当流式路径缺失（事件丢失/顺序异常）且消息未封口时才补填，
    // 绝不追加新消息——否则 final_answer 会作为第二遍重复显示。
    if (!messages.empty() && messages.back().role == MsgRole::Assistant &&
        !messages.back().sealed) {
        auto& m = messages.back();
        if (m.text.empty() && !a.final_answer.empty()) m.text = a.final_answer;
        m.sealed = true;
        m.streaming = false;
        m.duration_ms = a.total_duration_ms;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionSetBusy& a) {
    busy = a.busy;
    tabs.busy = a.busy;
    if (a.busy) {
        // 为即将到来的流式 token 预留一条 assistant 消息
        (void)active_stream();
        messages.back().streaming = false;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionPermissions& a) {
    if (sidebar.permission == a.label) return false;
    sidebar.permission = a.label;
    return true;
}

bool ViewModel::apply_variant(const ActionSetMode& a) {
    if (sidebar.mode == a.label) return false;
    sidebar.mode = a.label;
    return true;
}

bool ViewModel::apply_variant(const ActionAskUser&) { return true; }
bool ViewModel::apply_variant(const ActionAskUserTimeout&) { return true; }

// ActionOpenPlan 由 App::drain 直接消费（打开侧边栏预览），ViewModel 不处理
bool ViewModel::apply_variant(const ActionOpenPlan&) { return true; }

bool ViewModel::apply_variant(const ActionCacheDiagnostics& a) {
    // 仅 prefix_changed 时提示（对齐 src/tui ChatRenderer 语义）
    if (!a.prefix_changed) return false;
    std::string reason_str;
    for (size_t i = 0; i < a.reasons.size(); ++i) {
        if (i > 0) reason_str += "+";
        reason_str += a.reasons[i];
    }
    prompt_echo = std::string(str::kCachePrefixChanged) + (reason_str.empty() ? "?" : reason_str) +
                  std::string(str::kCacheMissSep) + std::to_string(a.cache_miss_tokens) +
                  std::string(str::kCacheTokensUnit);
    return true;
}

bool ViewModel::apply_variant(const ActionCompactionPaused& a) {
    if (!a.notice.empty())
        prompt_echo = a.notice;
    else if (a.paused)
        prompt_echo = std::string(str::kCompactPausedPrefix) +
                      std::to_string(a.consecutive_compacts) +
                      std::string(str::kCompactPausedSuffix);
    else
        prompt_echo = std::string(str::kCompactResumed);
    return true;
}

bool ViewModel::apply_variant(const ActionQueueUpdate& a) {
    // 消息队列更新：替换为最新快照（空 = 已清空/已发送，卡片消失）
    message_queue.items = a.items;
    // 队列清空时复位展开态（避免下次入队残留展开状态）
    if (message_queue.items.empty()) {
        message_queue.expanded = false;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionSubAgentProgress& a) {
    // 子任务进度：不混入主转录区，存入第二层独立记录（sub_records）
    if (a.step_type == "final") {
        // final 步由 SubAgentCompleted 处理，这里不重复
        return false;
    }

    // 第二层：完整记录（独立渲染）
    auto it = std::find_if(sub_records.begin(), sub_records.end(),
                           [&](const SubAgentDetail& s) { return s.task_id == a.task_id; });
    if (it == sub_records.end()) {
        sub_records.push_back(SubAgentDetail{});
        it = std::prev(sub_records.end());
        it->task_id = a.task_id;
    }
    it->status = "running";
    it->step_number = a.step_number;

    if (a.step_type == "observation") {
        // 观察结果合并到最近的 action 步骤（工具卡结果）；无匹配则独立记录
        bool merged = false;
        for (auto rit = it->steps.rbegin(); rit != it->steps.rend(); ++rit) {
            if (rit->step_type == "action" && !rit->done) {
                rit->observation = a.observation;
                rit->is_error = a.is_error;
                rit->done = true;
                rit->expanded = true;  // 第二层详情视图默认展开（结果可见）
                merged = true;
                break;
            }
        }
        if (!merged) {
            SubAgentStep st;
            st.step_number = a.step_number;
            st.step_type = a.step_type;
            st.content = a.content;
            st.observation = a.observation;
            st.is_error = a.is_error;
            st.done = true;
            st.expanded = true;
            it->steps.push_back(std::move(st));
        }
    } else {
        SubAgentStep st;
        st.step_number = a.step_number;
        st.step_type = a.step_type;
        st.content = a.content;
        st.thought_text = a.thought_text;
        st.tool_name = a.tool_name;
        st.tool_input = a.tool_input;
        st.duration_ms = a.duration_ms;
        st.expanded = true;  // 第二层详情视图默认展开（思考/工具内容可见）
        it->steps.push_back(std::move(st));
    }

    // 侧边栏任务调度 tab 聚合（轻量条目）
    auto lit = std::find_if(tabs.sub_agents.begin(), tabs.sub_agents.end(),
                            [&](const SubAgentLite& s) { return s.task_id == a.task_id; });
    if (lit == tabs.sub_agents.end()) {
        tabs.sub_agents.push_back(SubAgentLite{});
        lit = std::prev(tabs.sub_agents.end());
        lit->task_id = a.task_id;
    }
    lit->status = "running";
    lit->step_number = a.step_number;
    lit->current_step = a.step_type;

    // 关联到最近的 Agent 工具卡（sub_task_id 为空时回填，供点击跳转第二层）
    for (std::size_t i = messages.size(); i-- > 0;) {
        auto& m = messages[i];
        for (auto& t : m.tool_calls) {
            if (t.tool_name == "Agent" && t.running && t.sub_task_id.empty()) {
                t.sub_task_id = a.task_id;
                return true;
            }
        }
    }
    for (std::size_t i = messages.size(); i-- > 0;) {
        auto& m = messages[i];
        for (auto& t : m.tool_calls) {
            if (t.tool_name == "Agent" && t.sub_task_id.empty()) {
                t.sub_task_id = a.task_id;
                return true;
            }
        }
    }
    return true;
}

bool ViewModel::apply_variant(const ActionSubAgentCompleted& a) {
    // 第二层：更新状态/耗时/最终答复（不混入主转录区）
    auto it = std::find_if(sub_records.begin(), sub_records.end(),
                           [&](const SubAgentDetail& s) { return s.task_id == a.task_id; });
    if (it == sub_records.end()) {
        sub_records.push_back(SubAgentDetail{});
        it = std::prev(sub_records.end());
        it->task_id = a.task_id;
    }
    it->status = a.was_error ? "failed" : "done";
    it->duration_ms = a.duration_ms;
    it->final_answer = a.final_answer;

    // 侧边栏任务调度 tab 聚合
    auto lit = std::find_if(tabs.sub_agents.begin(), tabs.sub_agents.end(),
                            [&](const SubAgentLite& s) { return s.task_id == a.task_id; });
    if (lit == tabs.sub_agents.end()) {
        tabs.sub_agents.push_back(SubAgentLite{});
        lit = std::prev(tabs.sub_agents.end());
        lit->task_id = a.task_id;
    }
    lit->status = a.was_error ? "failed" : "done";
    lit->duration_ms = a.duration_ms;
    lit->current_step = a.was_error ? "failed" : "done";
    return true;
}

bool ViewModel::apply_variant(const ActionHookProgress& a) {
    // 按 hook_id 关联同一条 hook 的 start / done(failed) 两拍，合并为一行
    auto it = std::find_if(hook_progress.begin(), hook_progress.end(),
                           [&](const HookRow& r) { return r.hook_id == a.hook_id; });
    HookRow row;
    row.hook_id = a.hook_id;
    row.event = a.event;
    row.hook_type = a.hook_type;
    row.tool_name = a.tool_name;
    row.label = a.hook_label;
    row.phase = a.phase;
    row.message = a.message;
    if (it == hook_progress.end()) {
        hook_progress.push_back(std::move(row));
    } else {
        *it = std::move(row);
    }
    // FIFO 淘汰最旧条目（历史 done/failed 行随新事件滚动离开，保持面板精简）
    if (hook_progress.size() > kMaxHookRows)
        hook_progress.erase(hook_progress.begin(),
                            hook_progress.begin() +
                                static_cast<std::ptrdiff_t>(hook_progress.size() - kMaxHookRows));
    return true;
}

bool ViewModel::apply_variant(const ActionShutdown&) {
    pending_exit = true;
    return true;
}

bool ViewModel::apply_variant(const ActionToast& a) {
    prompt_echo = a.text;
    return true;
}

bool ViewModel::apply_variant(const ActionModelsLoaded&) {
    return false;  // 由 App 消费（刷新模型列表），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionSessionsLoaded&) {
    return false;  // 由 App 消费（填充会话搜索条目），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionProviderSwitched&) {
    return false;  // 由 App 消费（运行时热切换后端），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionProviderSwitchFailed&) {
    return false;  // 由 App 消费（提示切换失败），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionTodoUpdate& a) {
    if (sidebar.todos == a.todos) return false;  // 无变化，避免无谓重绘
    sidebar.todos = a.todos;
    return true;
}

bool ViewModel::apply_variant(const ActionMcpStatus& a) {
    // 结构化存储：状态点 + 失败错误信息（#27 M4）
    std::vector<McpServerEntry> entries;
    entries.reserve(a.servers.size());
    for (const auto& s : a.servers) {
        entries.push_back(McpServerEntry{
            s.name,
            s.protocol,
            s.tool_count,
            s.state,
            s.error,
        });
    }
    if (sidebar.mcp_servers == entries) return false;  // 无变化，避免无谓重绘
    sidebar.mcp_servers = std::move(entries);
    return true;
}

namespace {

/// @brief 复制时合并新树到旧树：目录递归继承旧展开状态（按 rel_path 匹配）
void merge_project_expand(const std::vector<ProjectNode>& src, std::vector<ProjectNode>& dst) {
    for (auto& d : dst) {
        if (!d.is_dir) continue;
        // 在旧树中找同路径目录，继承其 expanded
        for (const auto& s : src) {
            if (s.is_dir && s.rel_path == d.rel_path) {
                d.expanded = s.expanded;
                break;
            }
        }
        if (!d.children.empty()) {
            for (const auto& s : src)
                if (s.is_dir && s.rel_path == d.rel_path && !s.children.empty()) {
                    merge_project_expand(s.children, d.children);
                    break;
                }
        }
    }
}

}  // namespace

bool ViewModel::apply_variant(const ActionProjectFiles& a) {
    if (!a.loading) {
        // 复制后合并保留既有目录展开状态，避免 git 刷新后目录全部重开
        std::vector<ProjectNode> merged = a.tree;
        merge_project_expand(tabs.project.tree, merged);
        tabs.project.tree = std::move(merged);
    }
    if (!a.root.empty()) tabs.project.root = a.root;
    tabs.project.is_git = a.is_git;
    tabs.project.loading = a.loading;
    tabs.project.ready = a.loading ? tabs.project.ready : true;
    return true;
}

namespace {

/// @brief 按行切分（兼容 \r\n / \n），空串返回空列表
std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    if (content.empty()) return lines;
    std::string cur;
    for (const char c : content) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

/// @brief 截断到 max 字符（超长加省略号；回退到多字节边界避免切断 UTF-8）
std::string truncate_utf8(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    s.resize(max);
    // 去掉续字节回到字符首字节；若首字节是不完整多字节字符则一并去掉
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
    s += "…";
    return s;
}

/// @brief 从 reasoning 提取修改目的：取最后一行（去空白 + 截断 40）
std::string purpose_from_reasoning(const std::string& reasoning) {
    if (reasoning.empty()) return {};
    std::string last = reasoning;
    while (!last.empty() && (last.back() == '\n' || last.back() == '\r'))
        last.pop_back();  // 去掉末尾换行，避免取到空行
    const std::size_t pos = last.find_last_of('\n');
    if (pos != std::string::npos) last = last.substr(pos + 1);
    const auto is_space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!last.empty() && is_space(last.front())) last.erase(last.begin());
    while (!last.empty() && is_space(last.back())) last.pop_back();
    return truncate_utf8(std::move(last), 40);
}

}  // namespace

void ViewModel::track_file_change(const ActionBeginTool& a) {
    nlohmann::json args;
    try {
        args = nlohmann::json::parse(a.arguments);
    } catch (const std::exception&) {
        return;
    }
    if (!args.contains("file_path") || !args["file_path"].is_string()) return;

    FileChange ch;
    ch.file_path = args["file_path"].get<std::string>();
    if (a.tool_name == "Edit") {
        if (args.contains("old_string") && args["old_string"].is_string())
            ch.old_string = args["old_string"].get<std::string>();
        if (args.contains("new_string") && args["new_string"].is_string())
            ch.new_string = args["new_string"].get<std::string>();
    } else if (a.tool_name == "Write") {
        if (args.contains("content") && args["content"].is_string())
            ch.new_string = args["content"].get<std::string>();
    }
    if (ch.new_string.empty() && ch.old_string.empty()) return;

    // purpose：当前消息 reasoning 最后一行；空则回退 new_string 首行（R1）
    const auto& m = active_stream();
    ch.purpose = purpose_from_reasoning(m.reasoning);
    ch.reasoning = m.reasoning;  // 完整 reasoning（变更记录 tab 按 e 展开）
    if (ch.purpose.empty()) {
        const auto new_lines = split_lines(ch.new_string);
        if (!new_lines.empty()) ch.purpose = truncate_utf8(new_lines.front(), 40);
    }

    ch.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
    // active_stream() 保证 messages 非空，但显式检查防止重构后下溢
    ch.msg_index = messages.empty() ? 0 : messages.size() - 1;
    ch.diff = agent::line_diff(split_lines(ch.old_string), split_lines(ch.new_string), 1);

    tabs.changes.changes.push_back(std::move(ch));
    tabs.changes_open = true;
}

}  // namespace ftxtui