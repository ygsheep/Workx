/**
 * @file app.h
 * @brief App — 顶层组件树与事件驱动装配
 * @details 持有 ViewModel / ActionQueue / EventBridge，构建 ftxui 组件树，
 *          每帧 drain action 并重绘。输入经回调交回 main 的会话处理。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>

#include "core/events/agent_events.h"
#include "agent/tool/context.h"           // tool::PermissionMode
#include "agent/api/backend_types.h"      // agent::ModelInfo（模型切换 context_length）
#include "agent/factory.h"                // agent::BackendCreateResult
#include "agent/model/provider_config.h"  // agent::ProviderConfigEntry
#include "bridge/action_queue.h"
#include "bridge/event_bridge.h"
#include "render/markdown_to_elements.h"  // CardHit
#include "vm/view_model.h"
#include "widgets/suggest_panel.h"
#include "widgets/search_palette.h"
#include "widgets/sidebar_tabs.h"
#include "widgets/input_history.h"
#include "core/utils/file_index.h"  // agent::FileIndex::Entry

namespace agent {
class ChatSession;
class IBackendAdmin;
class ICompletionProvider;
class IEventBus;
class IConfigManager;
class ModelCatalog;
}  // namespace agent

namespace agent::mcp {
class McpClientManager;
}  // namespace agent::mcp

namespace agent::command {
class CommandRegistry;
}  // namespace agent::command

namespace agent::input {
class InputProcessor;
}  // namespace agent::input

namespace ftxtui {

/// @brief App 依赖（从 main 注入）
struct AppDeps {
    agent::ChatSession* session = nullptr;
    agent::IBackendAdmin* backend_admin = nullptr;
    agent::IEventBus* event_bus = nullptr;
    agent::IConfigManager* config_manager = nullptr;  ///< 配置（供应商列表 / 当前 provider）
    agent::ITaskManager* task_manager = nullptr;  ///< 后台任务（任务调度 tab 只读查询）
    bool mock_mode = false;   ///< --mock：无后端演示（send_input 走本地 mock 流）
    bool smoke_mode = false;  ///< --smoke：自动驱动一轮 mock 对话后退出（CI 冒烟）
    std::string model_name;
    std::string project;
    std::string agent_name;
    std::string session_dir;  ///< 项目会话目录（/resume 列出历史用）
    /// @brief 上下文窗口（token），启动时经 resolve_context_length 解析注入侧栏进度条分母
    int context_limit = 0;
    /// @brief models.dev 目录（原子指针，后台刷新；模型切换时重解析 context_length）
    std::shared_ptr<std::atomic<std::shared_ptr<const agent::ModelCatalog>>> model_catalog;
    /// @brief 命令注册表（内置 + 用户命令；斜杠命令经统一命令执行路径）
    std::shared_ptr<agent::command::CommandRegistry> command_registry;

    /// @brief MCP 连接管理器（#27 M4：侧栏展示已连接 server 状态）
    std::shared_ptr<agent::mcp::McpClientManager> mcp_manager;

    /// @brief 提交一条用户消息（由 main 的实现路由到会话/处理器）
    /// @param text 用户文本
    /// @param images 图片附件绝对路径（@ 图片引用自动提取，可为空）
    std::function<void(const std::string&, const std::vector<std::string>&)> on_submit;

    /// @brief 请求模型切换后回调（main 注入，刷新侧栏/状态行）
    std::function<void()> on_model_changed;

    /// @brief 退出应用回调（/exit）
    std::function<void()> on_exit;

    /// @brief 运行时创建后端（/provider 热切换；main 注入 agent::create_backend_for_entry 包装）
    /// @param entry 目标供应商条目（含 base_url/api_key/model，创建后端时以条目为准）
    /// @return BackendCreateResult（provider 可为空 = 配置不足或创建/初始化失败）
    std::function<agent::BackendCreateResult(const agent::ProviderConfigEntry& entry)>
        create_provider;

    /// @brief 配置落盘回调（供应商切换成功持久化后调用；main 注入 save_to_file）
    /// @details apply_provider_switch 只改 ConfigManager 内存，不落盘。
    ///          热切换后必须 save 到磁盘，否则重启读取旧配置还原为上一供应商。
    std::function<void()> save_config;
};

/// @brief 顶层应用
class App {
   public:
    explicit App(AppDeps deps);
    ~App();

    /// @brief 运行 ftxui 主循环（阻塞）
    void run();

    /// @brief 退出码（冒烟模式：0=通过，1=超时/失败；常规模式恒 0）
    int exit_code() const { return m_exit_code.load(); }

   private:
    void drain();
    void handle_ask_user(const ActionAskUser& a);
    void close_ask(bool submitted);
    /// @brief 记录当前题答案并前进；全部答完则提交（Enter 确认路径）
    bool advance_ask();
    /// @brief 冒烟驱动（B5）：自动投递 mock 对话，完成后请求退出
    void start_smoke_driver();
    /// @brief 发送用户输入（B2 输入链单一入口）
    /// @param text 用户文本
    /// @param force_flush 模型忙碌时是否请求下个工具轮边界立即冲刷（Ctrl+Enter）
    void send_input(const std::string& text, bool force_flush = false);
    /// @brief 技能命令任意位置调用：回显原始输入 + 注入合成 Skill 卡片 + 路由模型
    void handle_skill_invocation(const std::string& raw_input, const std::string& name,
                                 const std::string& args);
    /// @brief 统一命令执行入口（斜杠命令经 InputProcessor → CommandExecutor）
    void run_command(const std::string& cmd, const std::string& args);
    /// @brief ！命令执行（输入以 '!' 开头）：跨平台 Shell 执行 + 合成 Bash 卡
    /// @param raw_input 含 '!' 前缀的原始输入（回显用）
    /// @param send_to_model true=Ctrl+Enter：再把结构化（命令/退出码/输出）提交给模型
    void run_shell_command(const std::string& raw_input, bool send_to_model);
    void start_mock_stream(const std::string& user_text);
    void cmd_resume(const std::string& args);
    void cmd_rename(const std::string& args);
    /// @brief /new：新建会话并切换（保留旧会话文件；设置面板 NewSession 动作共用）
    void cmd_new();
    /// @brief /clear：删除当前会话文件并新建会话（设置面板 Clear 动作共用）
    void cmd_clear();
    /// @brief /view：打开文件只读查看器（读取 + 行号 + 虚拟化滚动）
    void cmd_view(const std::string& args);
    /// @brief /edit：内嵌 nvim 编辑文件（WithRestoredIO 全屏切换，返回后重读）
    void cmd_edit(const std::string& args);
    /// @brief /nvim：启动 nvim（可选文件路径，WithRestoredIO 全屏切换）
    void cmd_nvim(const std::string& args);
    /// @brief Ctrl+G：打开系统默认编辑器编辑当前输入（写入 ~/.workx/prompt.md，
    ///        编辑器结束后读回并替换输入框内容）
    void edit_prompt();
    /// @brief Windows notepad 异步分支：后台线程轮询 Prompt 文件实时同步到输入框，
    ///        按 Esc（finish_prompt_editor）结束；规避 Win11 Store 版 notepad stub
    ///        进程提前退出导致无法以进程退出作为结束信号的问题。
    void start_prompt_editor_async();
    /// @brief 结束异步编辑会话（Esc）：停轮询线程 + 最后读回 Prompt 文件同步输入框
    void finish_prompt_editor();
    /// @brief UI 线程消费轮询线程的最新内容（Custom 事件）
    void drain_prompt_pending();
    /// @brief 读 Prompt 文件并归一化（剥 UTF-8 BOM、CRLF/孤立 CR→LF、去尾换行）
    static std::string load_prompt_file(const std::string& path);
    /// @brief 重读当前文件 tab 内容（/edit 返回后与磁盘保持一致）
    void reload_file();
    /// @brief /Test:askuser：弹出 AskUser 提问弹窗（开发调试 TUI 渲染/交互用）
    void cmd_test_askuser();
    /// @brief 恢复指定会话（switch_session + 历史载入；cmd_resume 与搜索面板共用）
    void resume_session(const std::string& file_path, const std::string& title);
    /// @brief 从当前会话重建转录区（resume 历史载入 / 压缩上下文后刷新共用）
    void load_session_transcript();
    /// @brief 手动压缩上下文（搜索面板「压缩上下文」与 /compact 命令共用）
    void compact_context();
    /// @brief 新建会话后重置 UI 状态（/clear 与 /new 共用：清空消息/子 Agent/变更/统计/标题）
    void reset_vm_for_new_session();
    void open_model_selector();
    void apply_model(int index);
    /// @brief 从 m_model_items 重建 /model 面板条目（active = 当前模型）
    void rebuild_model_entries();
    /// @brief 打开模式选择面板（Ctrl+P → 切换模式，与 /model 同款悬浮选择）
    void open_mode_selector();
    /// @brief 应用选中的工作模式（标准 / 计划 / 极简）
    void apply_mode(int index);
    /// @brief 重建模式面板条目（active = 当前模式，subtitle = 模式介绍）
    void rebuild_mode_entries();
    /// @brief 打开 /resume 会话选择面板（仅会话条目）
    void open_resume_palette();
    /// @brief 打开供应商管理面板（读配置 backend.providers）
    void open_provider_palette();
    /// @brief 供应商面板选中：后台创建后端 → 完成后热切换
    void switch_provider(int index);
    /// @brief 供应商切换完成（UI 线程）：set_provider + 保留对话 + 写配置
    void handle_provider_switched(std::unique_ptr<agent::ICompletionProvider> provider,
                                  const std::string& model_name,
                                  const agent::ProviderConfigEntry& entry);
    /// @brief 供应商切换失败提示
    void handle_provider_switch_failed(const std::string& provider_name);
    /// @brief 会话列表懒加载（后台线程 list_sessions → ActionSessionsLoaded）
    void ensure_sessions_loaded();
    /// @brief 从 m_providers 装配供应商面板条目（active = 当前 provider）
    void assemble_provider_entries();
    /// @brief 从输入缓冲推导提示面板状态并过滤（每帧 / 字符变化后调用）
    void update_suggest();
    /// @brief 提示面板移动选择（delta=±1；Tab 传 +1，到底循环回首）
    void suggest_move(int delta);
    /// @brief 提示面板确认（命令模式=补全；文件模式=插入 @路径 引用）
    /// @return true=已消费（不提交发送）
    bool suggest_accept();
    /// @brief 提示面板 Enter 确认（文件模式=插入 @路径 引用）
    bool suggest_enter();
    /// @brief 提示面板 Ctrl+Enter 确认（文件模式=插入 @路径 引用）
    bool suggest_enter_insert();
    /// @brief 关闭提示面板（Esc）
    void suggest_cancel();
    /// @brief 装配聚合搜索条目（功能/文件/会话/设置），会话未加载时触发后台加载
    std::vector<SearchEntry> assemble_search_entries();
    /// @brief 执行聚合搜索面板选中项（按类别分发）
    void apply_search_entry(int index);
    /// @brief 执行设置动作（搜索面板「设置」类）
    void run_setting(int action);
    ftxui::Element build_transcript(int width);
    ftxui::Element build_ask_modal() const;
    /// @brief 消息队列卡片（模型忙碌时前端入队的用户消息；输入框上方可折叠条）
    /// @details 折叠态单行摘要（条数 + Ctrl+Enter 提示）；展开态逐条预览 + ✕ 移除。
    ///          命中区写入 m_queue_hits（标题行切换展开，✕ 移除对应条目）。
    ftxui::Element build_queue_bar();
    /// @brief Hook 执行进度条（#50 M-2：输入区上方多行，展示 Command/HTTP/Prompt hook 实时状态）
    /// @details 单行一条 hook：状态图标（进行中 ● / 完成 ✓ / 失败 ✕）+ 事件名 + hook 类型 +
    /// 展示标签。
    ///          为空时返回 emptyElement（零占用，不干扰既有布局）。
    ftxui::Element build_hook_progress_elem() const;
    /// @brief hook 进度条占用的行数（0 = 空；供 build_transcript 扣除可视高度防滚动错位）
    int hook_progress_height() const;
    /// @brief 标题栏下的层级子列表（面包屑导航）：主会话 / 子 Agent 记录
    ftxui::Element build_breadcrumb();
    /// @brief 第二层：子 Agent 独立记录渲染（不混入主转录区）
    ftxui::Element build_sub_agent_view(int width);
    /// @brief 切换到第二层并查看指定子 Agent 记录（task_id 匹配；找不到则选最近一条）
    void show_sub_agent(const std::string& task_id);
    /// @brief 返回主会话层级
    void show_main_level();
    static std::string mode_label(agent::tool::PermissionMode m);
    /// @brief 会话工作模式 → 状态行标签（"standard" / "plan" / "minimal"）
    static std::string session_mode_label(agent::tool::SessionMode m);
    /// @brief 权限两态切换（手动审批 ↔ 完全访问；Shift+Tab / 设置面板）
    void toggle_permission();
    /// @brief 工作模式三态切换（标准 → 极简 → 计划 → 标准；Tab / Ctrl+T）
    void toggle_mode();
    /// @brief 触发「已复制 N 字符」短暂提示（底层单线程，1.5s 后自动清除后重绘）
    void flash_copy_message(std::size_t char_count);
    /// @brief 选区文本变化回调：缓存最新选中内容，供鼠标释放时写入剪贴板
    void on_selection_changed();
    /// @brief 重试指定助手消息：截断到其触发用户消息并重新生成
    void retry_message(int msg_idx);
    /// @brief 关闭侧边栏可开合 tab（变更记录/文件），返回任务调度
    void close_sidebar_tab(SidebarTab tab);
    /// @brief 调整侧边栏宽度（Ctrl+← 减小 / Ctrl+→ 增大，钳制在 [20, 50]）
    void adjust_sidebar_width(int delta);
    /// @brief 跳转转录区到选中子 Agent 关联消息（任务调度 tab Menu Enter）
    void jump_to_sub_agent();
    /// @brief 跳转文件 tab 到选中修改点对应行（变更记录 tab Enter）
    void jump_change_to_file();
    /// @brief 刷新后台任务列表（渲染时只读查询 TaskManager，仅进行中/排队中）
    void refresh_background_tasks();
    /// @brief 后台扫描项目文件树 + git 状态并推送到 UI（项目 tab；线程内 m_queue.push）
    void start_project_scan();
    /// @brief 点击项目文件行打开查看器（相对项目根路径 → /view）
    void open_project_file(const std::string& rel_path);
    /// @brief 项目树方向键/滚轮滚动（钳制并请求重绘）
    void scroll_project(int delta);

    AppDeps m_deps;
    ViewModel m_vm;
    ActionQueue m_queue;
    EventBridge m_bridge;
    ftxui::ScreenInteractive m_screen;
    /// @brief 命令处理器（B2 统一：所有输入经它分派；持有 agent 注册表）
    std::unique_ptr<agent::input::InputProcessor> m_command_processor;

    /// @brief 折叠卡片渲染后的屏幕位置（每帧由 build_transcript 重建）
    // 用 deque：reflect 持有 Box&（CardHit 元素引用），deque push_back 不使已有元素引用失效
    std::deque<CardHit> m_hits;

    /// @brief sealed 消息的渲染元素缓存（第 2 层：滚动复用，跳过 Markdown/高亮重建）
    // hits 用堆上 deque（unique_ptr）：缓存的 Element 内 reflect 持有 Box&，需稳定地址。
    // 注意：不能把 deque 直接作为成员放进 vector——MSVC 的 std::deque move 非 noexcept，
    //       vector 扩容时按拷贝搬移 deque，元素地址全部变化，reflect 的 Box& 悬垂 →
    //       SetBox 写坏 shared_ptr 的 _Ptr（resume 后重发消息崩溃 0x560000002A 根因）。
    struct MsgCacheEntry {
        ftxui::Element element;  ///< 缓存的渲染树（仅 sealed，帧无关）
        std::unique_ptr<std::deque<CardHit>> hits;  ///< 卡片命中（reflect 实时回写屏幕坐标）
        int width = -1;                             ///< 上次渲染宽度（resize 失效）
        std::uint64_t key = 0;  ///< 渲染内容指纹（长度+折叠状态，见 sealed_cache_key）
        bool has = false;
    };
    std::vector<MsgCacheEntry> m_msg_cache;

    /// @brief 会话/消息清空时整体失效元素缓存与高度缓存
    void invalidate_msg_cache();
    /// @brief 计算 sealed 消息的渲染指纹（折叠状态+各文本长度；长度相同内容不同属罕见）
    static std::uint64_t sealed_cache_key(const MessageNode& m, int width);
    /// @brief 计算消息的估计高度指纹（与宽度无关；sealed 后内容定稿即可缓存高度）
    static std::uint64_t height_fingerprint(const MessageNode& m);

    /// @brief 第 3 层 前缀和缓存：逐消息估计高度（sealed 复用，流式逐帧重估）
    std::vector<int> m_msg_height;
    std::vector<std::uint64_t> m_msg_height_ver;

    /// @brief 追加一行运行时日志（workx_tui.log，多线程安全）
    void log_run(std::string_view msg);
    std::mutex m_log_mutex;

    // 输入缓冲与面板状态
    std::string m_input_buffer;
    /// @brief 输入历史（上下箭头浏览 + JSON 落盘；路径 ~/.workx/history.json）
    InputHistory m_input_history;
    std::chrono::steady_clock::time_point m_last_ctrl_c{};  ///< 上次 Ctrl+C 时刻（1s 内连按退出）
    bool m_ctrl_c_hint = false;  ///< 状态栏「再次按 Ctrl+C 退出」提示是否显示
    std::chrono::steady_clock::time_point m_ctrl_c_hint_until;  ///< 提示显示截止时刻
    std::string m_ask_buffer;  ///< AskUser 模态输入（自定义输入模式用）
    bool m_palette_open = false;
    bool m_model_open = false;
    int m_scroll = 0;
    bool m_follow = true;  ///< 自动跟随最新内容

    // ---- 输入栏提示面板（/ 命令 · @ 文件）----
    size_t m_composer_cursor = 0;  ///< composer 光标（外部持有，供路径替换同步）
    SuggestMode m_suggest_mode = SuggestMode::None;
    std::vector<SuggestEntry> m_suggest_entries;           ///< 过滤后的候选
    std::vector<agent::FileIndex::Entry> m_suggest_files;  ///< 文件候选（payload 映射）
    int m_suggest_selected = -1;                           ///< 选中项下标（-1 = 无）
    /// @brief 提示面板候选行渲染后的屏幕 box（每帧重建；deque 保证 reflect 地址稳定）
    std::deque<ftxui::Box> m_suggest_hits;

    // ---- 消息队列卡片（模型忙碌时缓存用户输入；输入框上方可折叠条）----
    /// @brief 队列卡片命中区（每帧由 build_queue_bar 重建）
    /// @details CardHit.msg_idx = 队列条目下标；button = -1 标题行（切换展开/折叠），
    ///          button >= 0 该条目的 ✕ 移除按钮。
    std::deque<CardHit> m_queue_hits;

    // ---- 聚合搜索面板（Ctrl+P）----
    std::vector<PaletteCommand> m_palette_cmds;  ///< 命令条目（注册表派生，搜索/提示共用）
    std::vector<SearchEntry> m_search_entries;  ///< 面板打开时装配的条目（on_select 映射）
    std::vector<SessionLite> m_session_metas;  ///< 会话列表缓存（后台加载）
    bool m_sessions_loading = false;           ///< 会话列表正在后台加载

    // ---- 统一悬浮面板：/resume 会话 · /model 模型 · 模式选择 · /provider 供应商 ----
    std::vector<SearchEntry> m_session_entries;  ///< /resume 面板条目（仅会话）
    bool m_resume_open = false;
    std::vector<SearchEntry> m_model_entries;  ///< /model 面板条目（由 m_model_items 派生）
    std::vector<SearchEntry> m_mode_entries;  ///< 模式选择面板条目（标准/计划/极简 + 介绍）
    bool m_mode_open = false;
    bool m_provider_open = false;
    std::vector<agent::ProviderConfigEntry> m_providers;  ///< 配置中的供应商列表
    std::string m_current_provider;  ///< 当前供应商 id（backend.provider）

    // ---- 侧边栏布局 ----
    bool m_sidebar_left = false;  ///< 侧边栏居中位置（false=右，true=左）
    int m_sidebar_width = 35;  ///< 侧边栏宽度百分比（Ctrl+←/→ / 鼠标拖动，[20,80]）
    bool m_sidebar_resizing = false;  ///< 正在用鼠标拖动侧边栏分隔线调整宽度
    /// @brief 侧边栏 tab 栏命中区（每帧由 build_sidebar_tabs 重建；鼠标点击用）
    std::deque<TabHit> m_tab_hits;
    /// @brief 侧栏可折叠区块命中区（MCP/TODO 标题行；每帧由 append_sidebar_info 重建）
    std::deque<SectionHit> m_section_hits;
    /// @brief 项目文件树组件（项目 tab 可交互：点击目录/文件、滚轮滚动）
    ftxui::Component m_project_tree;
    ftxui::Box m_project_box;  ///< 项目树组件渲染 box（点击命中用；折叠时置空）
    /// @brief 项目树常驻后台扫描线程（首扫 + 项目 tab 可见时周期重扫，生命周期内 join）
    std::thread m_project_watch_thread;
    std::atomic<bool> m_project_watch_run{false};  ///< 扫描线程运行开关（置 false 请求退出）
    std::atomic<bool> m_project_tab_active{false};  ///< 项目 tab 是否可见（仅可见时周期重扫）

    // ---- 输出区域层级导航（标题栏下子列表）----
    /// @brief 层级子列表命中区（面包屑项：主会话/子 Agent；每帧由 build_breadcrumb 重建）
    std::deque<CardHit> m_breadcrumb_hits;
    int m_sub_scroll = 0;      ///< 第二层（子 Agent 记录）独立滚动位置
    bool m_sub_follow = true;  ///< 第二层自动跟随最新记录
    /// @brief 第二层卡片命中区（思考/工具卡；每帧由 build_sub_agent_view 重建）
    std::deque<CardHit> m_sub_hits;

    /// @brief 第二层（子 Agent 记录）虚拟化 + 缓存（与主转录区同机制）
    // 复用 MsgCacheEntry：每虚拟消息（思考卡/工具卡/最终答复）一条缓存条目；
    // 高度缓存 m_sub_height/m_sub_height_ver 同理。切换记录时整体失效重建。
    std::vector<MsgCacheEntry> m_sub_cache;
    std::vector<int> m_sub_height;
    std::vector<std::uint64_t> m_sub_height_ver;
    /// @brief m_sub_cache 当前所属记录 task_id（切换记录时失效重建）
    std::string m_sub_cache_task;

    // ---- 子 Agent 菜单（任务调度 tab 可交互）----
    std::vector<std::string> m_sub_entries;  ///< 菜单条目（每帧由 sub_agents 重建）
    ftxui::Component m_sub_menu;             ///< 纵向 Menu（可聚焦；Enter 跳转转录）
    ftxui::Box m_sub_box;  ///< 菜单渲染 box（点击命中用；折叠时置空）

    // ---- 变更记录组件（变更记录 tab 可交互）----
    ftxui::Component m_change_viewer;  ///< 修改点 Menu + hunk + 目的展开
    ftxui::Box m_change_box;           ///< 组件渲染 box（点击命中用；折叠时置空）

    // ---- 文件查看组件（文件 tab 可聚焦：↑↓/PgUp/PgDn/滚轮滚动）----
    ftxui::Component m_file_viewer;  ///< 文件查看器（聚焦时接收滚动键）
    ftxui::Box m_file_box;  ///< 文件查看器渲染 box（滚轮命中用；折叠时置空）

    // ---- AskUser 模态（B3：多问题 + 选项 + 自定义输入 + cancel_flag）----
    struct AskQuestion {
        std::string question;  ///< 完整问题文本（答案 map 的 key）
        std::string header;    ///< 短标签（可空，回退用 question）
        bool multi = false;    ///< 多选（Space 勾选；Enter 确认当前题）
        bool allow_custom_input = true;
        std::vector<std::string> options;  ///< 选项 label
    };
    bool m_ask_active = false;
    size_t m_ask_qindex = 0;                   ///< 当前问题下标
    int m_ask_sel = 0;                         ///< 当前选中选项下标（-1 = 无）
    std::vector<bool> m_ask_checked;           ///< 多选勾选状态（仅 multi 用）
    bool m_ask_custom = false;                 ///< 是否处于自定义输入模式
    std::vector<AskQuestion> m_ask_questions;  ///< 解析后的问题列表
    std::vector<std::pair<std::string, std::string>> m_ask_answers;  ///< 已答（question→answer）
    std::shared_ptr<std::promise<agent::AskUserResult>> m_ask_promise;
    std::shared_ptr<std::atomic<bool>> m_ask_cancel;  ///< 取消标志（工作线程超时置位）
    std::chrono::steady_clock::time_point m_ask_deadline;
    /// @brief /Test:askuser 标记：模态关闭后把答案回显为 assistant 消息（调试用）
    bool m_ask_test_echo = false;

    std::vector<std::string> m_model_items;  ///< 模型列表（/model 面板）
    std::vector<agent::ModelInfo>
        m_model_infos;          ///< list_models 完整信息（apply_model 取 context_length）
    int m_mock_perm_cycle = 0;  ///< mock 下 Shift+Tab 权限循环序号（""→bypass）
    int m_mock_mode_cycle = 0;  ///< mock 下模式切换循环序号（standard→minimal→plan）

    // 思考动画（busy 时推进帧并持续重绘）
    std::size_t m_anim_frame = 0;         ///< 动画帧号（UI 线程自增）
    std::atomic<bool> m_anim_run{false};  ///< 动画线程运行标志
    std::atomic<bool> m_busy{false};      ///< 忙标志（UI 线程写，动画线程读）
    std::mutex m_anim_mutex;              ///< 动画线程等待锁（IDLE 睡眠）
    std::condition_variable m_anim_cv;    ///< busy 变化唤醒动画线程
    std::thread m_anim_thread;            ///< 动画推进线程（busy 时发 Event::Custom）

    // mock 流式输出（后台线程逐步入队 token，模拟 LLM 流式回复）
    std::atomic<bool> m_stream_run{false};  ///< 流式线程运行标志
    std::thread m_stream_thread;            ///< 流式线程

    /// @brief ！命令执行线程（后台 exec + m_queue.push 合成 Bash 卡；析构 join）
    /// @details 串行执行（新命令启动前 join 旧命令），避免并发命令交错。
    std::thread m_cmd_thread;

    // 冒烟模式（B5）状态：driver 线程经 atomic 与 UI 线程通信
    std::atomic<int> m_exit_code{0};          ///< 退出码（0=通过；1=超时）
    std::atomic<bool> m_smoke_submit{false};  ///< 置位=UI 线程应投递冒烟消息
    std::atomic<bool> m_smoke_exit{false};    ///< 置位=UI 线程应请求退出
    std::atomic<size_t> m_msg_count{0};       ///< 消息数镜像（drain 更新，driver 只读）

    // 组件
    ftxui::Component m_transcript;
    ftxui::Component m_sidebar;
    ftxui::Component m_status;
    ftxui::Component m_composer;
    ftxui::Component m_ask_input;
    ftxui::Component m_palette_comp;
    ftxui::Component m_resume_comp;
    ftxui::Component m_model_comp;
    ftxui::Component m_mode_comp;
    ftxui::Component m_provider_comp;

    // ---- Ctrl+G Prompt 编辑（Windows notepad 异步：后台轮询 + Esc 收尾）----
    /// @brief 异步编辑会话进行中（UI 线程写，轮询线程读）
    std::atomic<bool> m_prompt_editing{false};
    std::thread m_prompt_watch_thread;  ///< Windows notepad 文件轮询线程
    std::mutex m_prompt_mutex;          ///< 保护下列字段
    std::string m_prompt_last;     ///< 轮询线程上次读到的文件内容（变化判定）
    std::string m_prompt_pending;  ///< 有待 UI 线程消费的最新内容
    bool m_prompt_pending_dirty = false;   ///< pending 尚未被 UI 消费
    std::string m_prompt_path;             ///< 本轮 Prompt 文件路径（start 时记录）
    void* m_prompt_editor_proc = nullptr;  ///< Windows notepad 进程句柄（finish 时关闭）
    /// @brief 轮询线程检测到记事本窗口关闭（非 stub）后置位，UI 线程消费并自动收尾
    std::atomic<bool> m_prompt_auto_done{false};

    // ---- 拖拽选中 → 复制剪贴板（FTXUI 原生 Selection + 系统剪贴板）----
    std::string m_selection_text;    ///< 最新选中文本（SelectionChange 回调维护）
    bool m_copy_flash = false;       ///< 复制提示是否显示（仅 UI 线程读写）
    std::size_t m_copy_flash_n = 0;  ///< 提示中的字符数
    std::chrono::steady_clock::time_point m_copy_flash_until;  ///< 提示过期时刻
    /// @brief 提示自清除线程：仅 sleep 后触发一次重绘，不触碰任何 App 成员（析构安全）
    std::thread m_copy_flash_thread;
};

}  // namespace ftxtui