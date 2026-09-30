/**
 * @file chat_session.cpp
 * @brief 对话状态机实现
 * @details 编排用户输入、ReAct 循环、流式事件发布、自动重试、会话持久化
 * @version 3.1.2
 * @date 2026-07
 */

#include "agent/core/chat_session.h"
#include "agent/core/react_loop.h"
#include "agent/core/agent_type.h"          // 0.6.x：#31 AgentType 枚举 + 别名解析
#include "agent/core/goal_guarded_agent.h"  // 0.6.x：#31 GoalGuardedAgent + parse_goal
#include "agent/core/query_engine.h"        // 0.6.x：QueryEngine 唯一编排入口
#include "agent/plan/plan_coordinator.h"    // #54：Plan Mode V2 五阶段协调器
#include "agent/tool/path_matcher.h"        // #54：~/.workx/plan 家目录展开
#include "agent/message/types.h"
#include "agent/tool/tool_kind.h"
#include "agent/tool/TodoStore/todo_store.h"  // #24：待办清单持久化接线
#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/registry.h"
#include "agent/skill/inclaude/conditional.h"
#include "agent/skill/inclaude/hooks.h"
#include "agent/mcp/mcp_client_manager.h"  // #56 方案 D：MCP 连接管理器成员
#include "agent/config/app_config.h"
#include "agent/tool/AgentTool/agent_tool.h"  // #54：SubAgentLaunchOptions / launch_sub_agent（explore 并行）
#include "agent/hook/hook_manager.h"    // #50 通用 Hook 事件系统：会话级 SessionStart/End
#include "agent/audit/audit_logger.h"   // 会话生命周期审计
#include "agent/util/git_checkpoint.h"  // #81：切会话时重置 git 检查点基线
#include "core/task/task_manager.h"
#include "core/config/config_manager.h"
#include "core/utils/uuid.h"  // 项目会话恢复：UUID 生成

#include "agent/api/i_backend.h"
#include "agent/api/i_stream_reader.h"
#include <format>
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <ctime>

namespace agent {

// ============================================================
// 内部辅助
// ============================================================

// L-1：infer_tool_type 已提升至 core/tool_kind.h/.cpp 作为公共纯函数，
//      此处不再保留匿名命名空间副本，直接使用 agent::tool::infer_tool_type。

namespace {

/// @brief 获取当前 ISO 8601 时间戳（UTC），用于会话持久化
/// @details 格式：YYYY-MM-DDTHH:MM:SSZ
std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// ============================================================
// #87：权限模式枚举 ↔ 字符串（permission 事件落盘格式）
// ============================================================
// 字符串而非整数落盘：与 system_prompt.reason（initial/changed/resume）风格一致，
// 便于人工排查会话轨迹；映射失败即视为非法值（走 fail-safe 从严分支）。
// 注：headless.cpp 有同构的 parse_permission_mode（CLI 参数解析用），两者取值集合必须一致。

std::string permission_mode_to_string(tool::PermissionMode m) {
    switch (m) {
        case tool::PermissionMode::Default:
            return "default";
        case tool::PermissionMode::AcceptEdits:
            return "accept-edits";
        case tool::PermissionMode::Plan:
            return "plan";
        case tool::PermissionMode::BypassPermissions:
            return "bypass-permissions";
    }
    return "default";
}

std::optional<tool::PermissionMode> permission_mode_from_string(const std::string& s) {
    if (s == "default") return tool::PermissionMode::Default;
    if (s == "accept-edits") return tool::PermissionMode::AcceptEdits;
    if (s == "plan") return tool::PermissionMode::Plan;
    if (s == "bypass-permissions") return tool::PermissionMode::BypassPermissions;
    return std::nullopt;
}

std::string session_mode_to_string(tool::SessionMode m) {
    switch (m) {
        case tool::SessionMode::Standard:
            return "standard";
        case tool::SessionMode::Plan:
            return "plan";
        case tool::SessionMode::Minimal:
            return "minimal";
    }
    return "standard";
}

std::optional<tool::SessionMode> session_mode_from_string(const std::string& s) {
    if (s == "standard") return tool::SessionMode::Standard;
    if (s == "plan") return tool::SessionMode::Plan;
    if (s == "minimal") return tool::SessionMode::Minimal;
    return std::nullopt;
}

}  // anonymous namespace

namespace {

/// @brief 提取消息中的 <file path="..."> 引用，加入 touch 收集器
/// @details 只识别显式 file 标签，避免裸路径误报
void extract_file_path_touches(const std::string& text, skill::TouchCollector& collector) {
    static const std::string kTag = "<file path=\"";
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t start = text.find(kTag, pos);
        if (start == std::string::npos) break;
        const size_t value_begin = start + kTag.size();
        const size_t end = text.find("\">", value_begin);
        if (end == std::string::npos) break;
        collector.add(text.substr(value_begin, end - value_begin));
        pos = end + 2;
    }
}

}  // anonymous namespace

// ============================================================
// ChatSession::ReActEventPublisher — 3.2 IReActObserver 实现
// ============================================================

ChatSession::ReActEventPublisher::ReActEventPublisher(IEventBus& bus, std::string session_id)
    : m_bus(bus), m_session_id(std::move(session_id)) {}

void ChatSession::ReActEventPublisher::on_thought(const ReActStep& step) {
    // 发布 AgentStepEvent
    // 注意：description 只用简短占位。流式期间 on_token 已通过
    // StreamTokenEvent 把 thought_text 完整渲染到终端，这里若再
    // 写完整文本会导致 UI 重复显示同一份内容。
    m_bus.publish_async(AgentStepEvent{.step_id = std::format("thought-{}", step.step_number),
                                       .step_number = step.step_number,
                                       .description = "(thinking)"});
    // P3：有 tool_use 时发布 StepDoneEvent（而非 StreamDoneEvent），
    // 让 UI 知道本轮 LLM 流式输出结束，但不触发会话级结束动作。
    // 原实现发布 StreamDoneEvent 导致语义污染：UI 误显示完成、
    // token 统计被 0 值覆盖、状态机错误转 IDLE、光标错位。
    if (!step.tool_uses.empty()) {
        m_bus.publish_async(StepDoneEvent{.session_id = m_session_id,
                                          .full_content = step.thought_text,
                                          .full_reasoning = step.reasoning,
                                          .generation_ms = step.duration_ms});
    }
}

void ChatSession::ReActEventPublisher::on_action(const ReActStep& step) {
    m_bus.publish_async(ToolCallEvent{.tool_name = step.tool_name,
                                      .arguments = step.tool_input.dump(),
                                      .call_id = step.tool_use_id,
                                      .tool_type = tool::infer_tool_type(step.tool_name)});
}

void ChatSession::ReActEventPublisher::on_observation(const ReActStep& step) {
    m_bus.publish_async(ToolResultEvent{
        .call_id = step.tool_use_id, .result = step.observation, .is_error = step.is_error});
}

void ChatSession::ReActEventPublisher::on_final_answer(const ReActStep& /*step*/) {
    // 不在此发布事件，循环结束后由 run_completion 统一处理
}

void ChatSession::ReActEventPublisher::on_token(const std::string& content_delta,
                                                const std::string& reasoning_delta) {
    m_bus.publish_async(StreamTokenEvent{.session_id = m_session_id,
                                         .content_delta = content_delta,
                                         .reasoning_delta = reasoning_delta,
                                         .is_thinking = !reasoning_delta.empty(),
                                         .token_count = 0});
}

// ============================================================
// 构造与析构
// ============================================================

ChatSession::ChatSession(std::unique_ptr<ICompletionProvider> provider, ITaskManager& task_manager,
                         IEventBus& event_bus, IConfigManager& config_manager, int retry_delay_ms,
                         std::string session_id)
    : m_provider(std::move(provider)),
      m_session_id(std::move(session_id)),
      m_cwd(std::filesystem::current_path().string()),
      m_task_manager(task_manager),
      m_event_bus(event_bus),
      m_config_manager(config_manager) {
    // H-3：从配置管理器读取重试配置，统一委托给 HttpRetryPolicy
    // 注意：仅当配置中显式设置时才覆盖，否则使用 preset 传入的值（可为不同 provider 设置不同延迟）
    auto& cfg = m_config_manager.get();
    m_retry_policy.max_retries =
        cfg.has("backend.retry_count") ? cfg.get_or<int>("backend.retry_count", 3) : 3;
    m_retry_policy.base_delay_ms = cfg.has("backend.retry_delay_ms")
                                       ? cfg.get_or<int>("backend.retry_delay_ms", retry_delay_ms)
                                       : retry_delay_ms;

    // DS_CACHE H-3：注册压缩器暂停回调，发布 CompactionPausedEvent 到 EventBus
    m_compactor.set_paused_callback([this](bool paused, int consecutive_compacts, int32_t tokens,
                                           float ratio, const std::string& notice) {
        m_event_bus.get().publish_async(CompactionPausedEvent{
            .session_id = m_session_id,
            .paused = paused,
            .consecutive_compacts = static_cast<int32_t>(consecutive_compacts),
            .tokens_before = tokens,
            .ratio = ratio,
            .notice = notice});
    });

    // DS_CACHE M-4：注入 LLM 摘要回调（compact 阶段调用，失败自动 fallback 到机械折叠）
    // 捕获 this 以访问 m_provider；provider 生命周期与 ChatSession 绑定，安全。
    m_compactor.set_summarize_fn([this](const std::vector<ChatMessage>& middle) {
        return this->summarize_with_llm(middle);
    });

    // #24：接线 TodoStore 事件总线（变更后发布 TodoUpdatedEvent → UI 侧边栏/StatusBar）
    tool::TodoStore::instance().set_event_bus(&m_event_bus.get());

    // #50：装配会话级 HookManager（SessionStart/SessionEnd 事件；复用循环级同一装配逻辑）。
    //      会话级无独立工具 registry，agent 类型 hook 在此降级为纯 prompt 判定（白名单可空）。
    m_hooks = hook::make_hook_manager(m_config_manager.get(), nullptr, m_provider.get(),
                                      &m_event_bus.get());

    subscribe_interrupt();
    subscribe_sub_agent_persistence();

    // #54：Plan Mode V2 协调器（会话语义级驱动五阶段 + 并行 explore 编排）
    m_plan_coordinator = std::make_unique<plan::PlanCoordinator>(m_config_manager.get());
    subscribe_plan_events();
}

ChatSession::~ChatSession() {
    // #50 SessionEnd hook：会话析构收尾时触发一次（switch_session 不触发，语义与现有
    //      持久化约定对齐——会话可被多次 resume 继续）。执行在本类成员仍存活期内。
    if (m_hooks && !m_hooks->empty() && !m_session_end_hook_fired) {
        m_session_end_hook_fired = true;
        hook::HookContext hctx;
        hctx.session_id = m_session_id;
        hctx.cwd = m_cwd;
        hctx.stop_reason = "session_ended";
        m_hooks->dispatch(hook::HookEvent::SessionEnd, hctx);
    }
    unsubscribe_sub_agent_persistence();
    unsubscribe_plan_events();
    unsubscribe_interrupt();
    if (m_provider) {
        m_provider->interrupt();
    }
    // P1-1：先定向取消本会话已分发的后台任务，避免其后台线程越过会话销毁访问裸指针
    cancel_background_tasks();
    // H-9：等待后台任务完成，防止 use-after-free
    // 改用 ITaskManager::wait(task) 替代 sleep_for(50ms) 轮询
    // wait 内部用 condition_variable.wait_until + 30s 兜底超时
    std::shared_ptr<Task> task;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        task = m_current_task;
    }
    if (task) {
        task->cancel();
        m_task_manager.get().wait(task);
    }
    // #26 评审 #2：cancel/join AgentTool 启动的子 Agent 任务，避免其后台线程
    // 在 m_provider 等成员销毁后仍访问已释放指针（use-after-free）。
    // 子 Agent 为协作式取消，cancelAll 后 waitForAll 会快速返回。
    m_task_manager.get().cancelAll();
    m_task_manager.get().waitForAll();
}

void ChatSession::cancel_background_tasks() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        ids.swap(m_background_task_ids);
    }
    if (ids.empty()) {
        return;
    }
    auto& tm = m_task_manager.get();
    for (const auto& id : ids) {
        if (const auto task = tm.find_task(id)) {
            task->cancel();  // 协作式取消：仅置原子标志，线程安全
        }
    }
}

void ChatSession::cancel_and_wait_current_task() {
    // H-9 wait 模式：先定向取消本会话后台任务（P1-1）+ cancel 当前任务 + wait
    // （condition_variable.wait_until + 30s 兜底），再 cancelAll + waitForAll 终止
    // AgentTool 启动的子 Agent 后台任务。
    // 全部等待完成后再改共享状态，避免与 ReActLoop 对 m_messages 的引用读写并发。
    cancel_background_tasks();
    std::shared_ptr<Task> task;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        task = m_current_task;
    }
    if (task) {
        task->cancel();
        m_task_manager.get().wait(task);
    }
    m_task_manager.get().cancelAll();
    m_task_manager.get().waitForAll();
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_generating.store(false);
    m_current_task.reset();
}

void ChatSession::set_system_prompt(const std::string& prompt) {
    std::string reason;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (prompt == m_system_prompt) return;
        m_system_prompt = prompt;
        // 已落盘过 → 运行时变更；首次设置 → 待懒创建 store 后补写 initial
        reason = m_system_prompt_recorded ? "changed" : "initial";
    }
    persist_system_prompt(reason);
}

void ChatSession::set_system_prompt_builder(std::function<std::string(tool::SessionMode)> builder) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_system_prompt_builder = std::move(builder);
}

std::string ChatSession::system_prompt() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_system_prompt;
}

void ChatSession::set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_tool_registry = std::move(registry);
    // executor 由 ReActLoop 内部创建，此处仅保存 registry
}

std::shared_ptr<tool::ToolRegistry> ChatSession::tool_registry() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_tool_registry;
}

void ChatSession::set_file_index_invalidator(std::function<void()> invalidator) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_file_index_invalidator = std::move(invalidator);
}

void ChatSession::set_command_registry(std::shared_ptr<command::CommandRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_command_registry = std::move(registry);
}

std::shared_ptr<command::CommandRegistry> ChatSession::command_registry() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_command_registry;
}

void ChatSession::set_mcp_manager(std::shared_ptr<mcp::McpClientManager> manager) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_mcp_manager = std::move(manager);
}

std::shared_ptr<mcp::McpClientManager> ChatSession::mcp_manager() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_mcp_manager;
}

skill::TouchCollector& ChatSession::touch_collector() { return m_touch_collector; }

void ChatSession::clear_history() {
    // 生成中安全：清空消息前先取消并等待当前任务，避免与 ReActLoop 竞争 m_messages。
    cancel_and_wait_current_task();
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages.clear();
    // DS_CACHE M-3：移除 m_cache_hit_total/m_cache_miss_total 重置（死代码已删除）
    // DS_CACHE M-5：重置压缩器状态（卡死守卫/rewrite_version），避免跨 clear_history 泄漏
    m_compactor.reset();
    // 重置前缀形状基线
    m_last_prefix_shape = PrefixShape{};
    // 清理 conditional skills 会话级累积：过期 touch 不再触发激活
    m_touch_collector.clear();
    m_activated_skills.clear();
    // #24：清空待办清单（写空快照到 JSONL 防止 /resume 恢复旧清单，保留持久化回调）
    tool::TodoStore::instance().reset_session(m_session_id);
    // 消息队列：清空待发送缓存（避免残留消息进入新历史/被误冲刷）
    clear_pending_queue();
    // 审计：会话结束（清空历史）
    audit::AuditLogger::instance().log_session_lifecycle(audit::EventType::SessionEnd,
                                                         m_session_id);
}

void ChatSession::set_compactor_context_window(int32_t context_window_tokens) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_compactor.set_context_window(context_window_tokens);
}

void ChatSession::set_compactor_archive_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_compactor.set_archive_dir(dir);
}

// ============================================================
// 项目会话恢复：SessionStore 集成
// ============================================================

void ChatSession::set_session_store(std::shared_ptr<agent::session::SessionStore> store) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_session_store = std::move(store);
}

void ChatSession::append_skill_event(const agent::session::SkillEvent& ev) {
    std::shared_ptr<agent::session::SessionStore> store;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
    }
    if (store) store->append_skill(ev);
}

void ChatSession::configure_session_store(const std::string& project_dir, const std::string& cwd,
                                          const std::string& model, const std::string& git_branch) {
    // 锁内仅做状态写入；SessionStart hook 派发在锁外执行（可长耗时，含 command/http/prompt）
    std::string disp_hook_session_id;
    std::string disp_hook_cwd;
    bool dispatch_start = false;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_store_configured = true;
        m_store_project_dir = project_dir;
        m_store_cwd = cwd;
        m_store_model = model;
        m_store_git_branch = git_branch;
        // 审计：会话开始（启动装配时必然调用，保证审计日志文件必然生成）
        audit::AuditLogger::instance().log_session_lifecycle(audit::EventType::SessionStart,
                                                             m_session_id);

        // #50 SessionStart hook：会话装配完成、首条消息前触发一次（每个 ChatSession 实例仅一次）
        if (m_hooks && !m_hooks->empty() && !m_session_start_hook_fired) {
            m_session_start_hook_fired = true;
            dispatch_start = true;
            disp_hook_session_id = m_session_id;
            disp_hook_cwd = m_store_cwd;
        }
    }
    if (dispatch_start) {
        hook::HookContext hctx;
        hctx.session_id = disp_hook_session_id;
        hctx.cwd = disp_hook_cwd;
        m_hooks->dispatch(hook::HookEvent::SessionStart, hctx);
    }
}

bool ChatSession::restore_from_file(const std::string& file_path) {
    auto messages = agent::session::SessionStore::load_messages(file_path);
    if (messages.empty()) return false;

    std::lock_guard<std::mutex> lock(m_state_mutex);
    // 追加到已有消息（不清空），支持恢复后继续对话
    m_messages.insert(m_messages.end(), std::make_move_iterator(messages.begin()),
                      std::make_move_iterator(messages.end()));
    return true;
}

CacheAwareCompactor::Result ChatSession::compact_context() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_compactor.maybe_compact(m_messages);
}

void ChatSession::import_messages(std::vector<ChatMessage> messages) {
    // 生成中安全：与 switch_session 同理，先取消并等待当前任务，避免与 ReActLoop 竞争 m_messages。
    cancel_and_wait_current_task();
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages = std::move(messages);
    // 重置压缩器与前缀形状基线（对齐 switch_session，新上下文从零开始）
    m_compactor.reset();
    m_last_prefix_shape = PrefixShape{};
}

bool ChatSession::set_provider(std::unique_ptr<ICompletionProvider> provider) {
    if (!provider) return false;
    std::lock_guard<std::mutex> lock(m_state_mutex);
    if (m_generating.load()) return false;  // 生成中拒绝切换（ReActLoop 持有 provider 指针）
    m_provider = std::move(provider);
    return true;
}

bool ChatSession::switch_session(
    const std::string& file_path) {  // 加载历史消息和元信息（文件 I/O 在锁外执行）
    // 生成中安全：切换会话前先取消并等待当前任务完全退出。
    // ReActLoop::run 通过非 const 引用直接读写 m_messages，若此处（UI 线程）并发 move
    // m_messages，会与任务线程形成数据竞争 → 堆损坏（resume 后重发消息崩溃的 UAF 根因）。
    cancel_and_wait_current_task();

    auto messages = agent::session::SessionStore::load_messages(file_path);
    if (messages.empty()) return false;

    auto meta = agent::session::SessionStore::load_meta(file_path);
    if (!meta) return false;

    // #87：权限模式快照（append-only，取最后一条；无事件时为空 → 走 fail-safe）
    // 与 load_messages/load_meta 同在锁外读取：文件 I/O 不应持 m_state_mutex。
    auto permission = agent::session::SessionStore::load_permission(file_path);

    // 从文件名提取 session_id（stem，如 "76e1b10d-...-...jsonl" → "76e1b10d-...-..."）
    std::string new_session_id = std::filesystem::path(file_path).stem().string();

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        // 1. 替换 session_id
        m_session_id = new_session_id;

        // 2. 清空消息历史，填入加载的历史消息
        m_messages = std::move(messages);

        // 3. 关闭旧 SessionStore，创建新 SessionStore 指向历史文件
        if (m_session_store) {
            m_session_store->close();
        }
        auto new_store = std::make_shared<agent::session::SessionStore>(file_path, new_session_id);
        if (!new_store->open()) {
            return false;  // 打开失败，保留旧状态不变（messages 已替换但 store 为空，后续不持久化）
        }
        m_session_store = new_store;
        // 不追加 session_start（会话进行中，只是换文件继续写）

        // 4. 重置压缩器和前缀形状基线（新会话上下文从零开始）
        m_compactor.reset();
        m_last_prefix_shape = PrefixShape{};

        // 5. #81：丢弃旧会话的 git 检查点基线（新会话下次 run 重新捕获）
        //    否则 /diff、/rollback 会沿用上一会话目录的 base commit，回滚到错误仓库。
        util::GitCheckpoint::instance().reset();

        // 6. #87：恢复权限模式与工作模式。
        //    修复前三态只存在于内存，resume 后一律重置为 Default：用户在 Plan 模式批准
        //    方案后中断再恢复，agent 会变成普通模式「可以写入和执行」，而用户以为还在
        //    只读阶段——安全边界静默降级。
        m_last_permission_restore = restore_permission_locked(permission);
    }  // 释放 m_state_mutex

    // #24：恢复该会话待办清单（发布事件刷新 UI）+ 接线持久化回调。
    // 必须在锁外执行：restore_todos 与 wire_todo_persistence 内部会再次加 m_state_mutex，
    // 持锁调用会对非递归 mutex 二次锁定 → EDEADLK（resume 会话崩溃根因）。
    auto todos = agent::session::SessionStore::load_todos(file_path);
    tool::TodoStore::instance().restore_todos(new_session_id, std::move(todos));
    wire_todo_persistence();
    // 会话轨迹调试：切换后记录当前系统提示词为 resume 快照（file 已打开）
    if (!m_system_prompt.empty()) {
        persist_system_prompt("resume");
    }
    // 审计：切换到恢复的会话
    audit::AuditLogger::instance().log_session_lifecycle(audit::EventType::SessionStart,
                                                         new_session_id);

    return true;
}

void ChatSession::new_session() {
    // 生成中安全：先取消并等待当前任务，避免与 ReActLoop 竞争 m_messages。
    cancel_and_wait_current_task();
    std::string new_session_id;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        // 1. 新会话 ID
        new_session_id = core::util::generate_uuid();
        m_session_id = new_session_id;
        // 2. 清空消息历史
        m_messages.clear();
        // 3. 关闭旧 SessionStore 并置空（新会话文件懒创建：首条 user 消息时创建）
        if (m_session_store) {
            m_session_store->close();
            m_session_store.reset();
        }
        // 4. 重置压缩器与前缀形状基线 + conditional skills 会话级累积
        m_compactor.reset();
        m_last_prefix_shape = PrefixShape{};
        m_touch_collector.clear();
        m_activated_skills.clear();
        // 新会话文件懒创建后补写 initial system_prompt 快照（会话轨迹调试）
        m_system_prompt_recorded = false;
        m_pending_system_prompt = !m_system_prompt.empty();
        // 消息队列：清空待发送缓存（新会话从空队列开始）
        clear_pending_queue();
        // 懒创建参数（m_store_configured 等）保持不变，复用启动时配置
    }  // 释放 m_state_mutex

    // 锁外：重置 TodoStore 到新会话空清单（restore_todos 内部会再加 m_state_mutex，
    // 持锁调用会对非递归 mutex 二次锁定 → EDEADLK，与 switch_session 同理）。
    tool::TodoStore::instance().restore_todos(new_session_id, {});
    wire_todo_persistence();
    // 审计：新会话开始
    audit::AuditLogger::instance().log_session_lifecycle(audit::EventType::SessionStart,
                                                         new_session_id);
}

void ChatSession::wire_todo_persistence() {
    std::shared_ptr<agent::session::SessionStore> store;
    std::string session_id;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
        session_id = m_session_id;
    }
    if (!store) return;
    tool::TodoStore::instance().set_persist_callback(
        session_id,
        [store](const std::vector<core::todo::TodoItem>& todos) { store->append_todo(todos); });
}

bool ChatSession::rename_session(const std::string& title) {
    std::shared_ptr<agent::session::SessionStore> store;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
    }
    if (!store) return false;
    return store->append_title(title);
}

void ChatSession::persist_message(const ChatMessage& msg) {
    std::shared_ptr<agent::session::SessionStore> store;
    bool is_first_user = false;
    bool need_lazy_init = false;
    std::string lazy_project_dir, lazy_cwd, lazy_model, lazy_git_branch;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;

        // 检测是否是首条 user 消息（用于自动生成标题 + 懒创建触发条件）
        // 注意：当前 msg 在 persist_message 调用前已被 push_back 到 m_messages 中，
        //       因此用 user 消息计数 == 1 判断首条（而非检查"是否有 user 消息"）
        if (msg.role == ChatMessage::Role::User) {
            int user_count = 0;
            for (const auto& m : m_messages) {
                if (m.role == ChatMessage::Role::User) ++user_count;
            }
            is_first_user = (user_count == 1);
        }

        // 懒创建：首条 user 消息且 SessionStore 未创建时，创建文件 + 写 session_start
        if (!store && m_store_configured && is_first_user) {
            need_lazy_init = true;
            lazy_project_dir = m_store_project_dir;
            lazy_cwd = m_store_cwd;
            lazy_model = m_store_model;
            lazy_git_branch = m_store_git_branch;
        }

        if (!store && !need_lazy_init) return;
    }

    // 懒创建 SessionStore（锁外执行文件 I/O）
    if (need_lazy_init) {
        try {
            namespace fs = std::filesystem;
            fs::path session_file = fs::path(lazy_project_dir) / (m_session_id + ".jsonl");
            auto new_store =
                std::make_shared<agent::session::SessionStore>(session_file.string(), m_session_id);
            if (!new_store->open()) return;
            new_store->append_session_start(lazy_cwd, lazy_model, lazy_git_branch);
            // 写入 store 后再持久化消息
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_session_store = new_store;
            }
            store = new_store;
            // 补写待记录的 initial system_prompt 快照（会话轨迹调试）
            if (m_pending_system_prompt) {
                persist_system_prompt("initial");
            }
            // #24：接线 TodoStore 持久化回调（新会话从空清单开始）
            wire_todo_persistence();
        } catch (const std::exception&) {
            return;  // 创建失败，放弃持久化
        }
    }

    if (!store) return;

    const std::string uuid = core::util::generate_uuid();
    const std::string timestamp = now_iso();

    switch (msg.role) {
        case ChatMessage::Role::User:
            store->append_user_message(uuid, "", msg.content, timestamp);
            // 首条 user 消息自动生成标题（前 20 字）
            if (is_first_user) {
                std::string title = msg.content;
                // UTF-8 安全截取前 20 字
                size_t char_count = 0;
                size_t byte_pos = 0;
                while (char_count < 20 && byte_pos < title.size()) {
                    unsigned char c = static_cast<unsigned char>(title[byte_pos]);
                    if (c < 0x80)
                        byte_pos += 1;
                    else if ((c & 0xE0) == 0xC0)
                        byte_pos += 2;
                    else if ((c & 0xF0) == 0xE0)
                        byte_pos += 3;
                    else if ((c & 0xF8) == 0xF0)
                        byte_pos += 4;
                    else
                        byte_pos += 1;
                    ++char_count;
                }
                title = title.substr(0, byte_pos);
                if (byte_pos < msg.content.size()) {
                    title += "...";
                }
                store->append_title(title);
            }
            break;
        case ChatMessage::Role::Assistant:
            store->append_assistant_message(uuid, "", msg.content, msg.reasoning_content,
                                            msg.tool_uses, timestamp, msg.reasoning_ms);
            break;
        case ChatMessage::Role::Tool:
            store->append_tool_message(uuid, "", msg.tool_call_id, msg.tool_name, msg.content,
                                       msg.is_error, timestamp);
            break;
        default:
            break;
    }
}

void ChatSession::persist_messages_range(size_t start_idx, const std::string& parent_uuid) {
    std::shared_ptr<agent::session::SessionStore> store;
    std::vector<ChatMessage> messages_copy;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
        if (!store) return;
        if (start_idx >= m_messages.size()) return;
        messages_copy.assign(m_messages.begin() + start_idx, m_messages.end());
    }

    std::string current_parent = parent_uuid;
    for (const auto& msg : messages_copy) {
        const std::string uuid = core::util::generate_uuid();
        const std::string timestamp = now_iso();

        switch (msg.role) {
            case ChatMessage::Role::User:
                store->append_user_message(uuid, current_parent, msg.content, timestamp);
                break;
            case ChatMessage::Role::Assistant:
                store->append_assistant_message(uuid, current_parent, msg.content,
                                                msg.reasoning_content, msg.tool_uses, timestamp,
                                                msg.reasoning_ms);
                break;
            case ChatMessage::Role::Tool:
                store->append_tool_message(uuid, current_parent, msg.tool_call_id, msg.tool_name,
                                           msg.content, msg.is_error, timestamp);
                break;
            default:
                break;
        }
        current_parent = uuid;  // 链式传递 parent_uuid
    }
}

void ChatSession::persist_system_prompt(const std::string& reason) {
    std::shared_ptr<agent::session::SessionStore> store;
    std::string prompt;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
        prompt = m_system_prompt;
        if (!store) {
            // store 未创建（首条 user 消息前）：置 pending，懒创建后补写 initial 快照
            m_pending_system_prompt = true;
            return;
        }
    }
    store->append_system_prompt(reason, prompt);
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_system_prompt_recorded = true;
    m_pending_system_prompt = false;
}

std::string ChatSession::summarize_with_llm(const std::vector<ChatMessage>& middle) {
    // DS_CACHE M-4：同步调用 LLM 生成中段摘要
    // 由 compact_middle 在 compact 阶段调用；失败抛异常，由调用方 fallback 到机械折叠
    if (!m_provider) {
        throw std::runtime_error("summarize_with_llm: provider is null");
    }
    if (middle.empty()) {
        throw std::runtime_error("summarize_with_llm: empty middle");
    }

    // 构造摘要请求：system prompt 指示摘要任务 + middle 消息
    static const std::string k_summary_system_prompt =
        "你是对话历史压缩助手。请将以下历史对话（含用户问题、助手回答、工具调用结果）"
        "压缩为一份简洁的结构化摘要，保留：\n"
        "1. 用户的核心意图与关键约束\n"
        "2. 已完成的关键操作及其结果（工具名 + 要点）\n"
        "3. 未解决的问题或待办事项\n"
        "4. 关键文件路径、错误信息、数值参数\n"
        "用 <compaction-summary> 标签包裹摘要内容。不超过 500 字。不要添加新信息。";

    CompletionRequest req;
    // system prompt 作为首条消息（对齐主推理流程的 messages 约定）
    req.messages.reserve(middle.size() + 1);
    req.messages.push_back(ChatMessage::system(k_summary_system_prompt));
    for (const auto& msg : middle) {
        // 剥离图片附件：摘要请求只发文本（压缩模型可能不支持视觉）
        ChatMessage copy = msg;
        copy.image_paths.clear();
        req.messages.push_back(std::move(copy));
    }
    req.max_tokens = 1024;   // 摘要无需过长
    req.temperature = 0.3f;  // 低温度保证忠实
    req.stream = true;

    // 提交流式请求
    auto reader = m_provider->submit_completion(req);
    if (!reader) {
        throw std::runtime_error("summarize_with_llm: submit_completion returned null");
    }

    // 阻塞读取流直到完成，拼接 content
    std::string summary;
    StreamChunk chunk;
    auto should_stop = []() { return false; };  // 摘要不响应取消信号（短任务）
    while (true) {
        auto state = reader->next(should_stop, chunk);
        if (state == StreamState::HasData) {
            if (!chunk.content_delta.empty()) {
                summary += chunk.content_delta;
            }
            if (chunk.is_final) {
                break;
            }
        } else if (state == StreamState::Complete) {
            break;
        } else if (state == StreamState::Error) {
            throw std::runtime_error("summarize_with_llm: stream error");
        } else if (state == StreamState::Cancelled) {
            throw std::runtime_error("summarize_with_llm: stream cancelled");
        }
    }

    if (summary.empty()) {
        throw std::runtime_error("summarize_with_llm: empty summary");
    }

    // 确保 <compaction-summary> 包裹（LLM 可能不严格遵循 system prompt）
    if (summary.find("<compaction-summary>") == std::string::npos) {
        summary = "<compaction-summary>\n" + summary + "\n</compaction-summary>";
    }

    return summary;
}

void ChatSession::regenerate() {
    // 检查是否正在生成
    if (m_generating.load()) {
        m_event_bus.get().publish_async(
            StreamErrorEvent{.session_id = m_session_id,
                             .message = "Still generating, cannot regenerate",
                             .retryable = true});
        return;
    }

    std::string last_user_text;
    std::vector<std::string> last_user_images;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        while (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::Assistant) {
            m_messages.pop_back();
        }
        if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::User) {
            last_user_text = m_messages.back().content;
            last_user_images = m_messages.back().image_paths;
            m_messages.pop_back();
        } else {
            return;  // 没有 user 消息可重生成
        }
    }
    run_completion(last_user_text, last_user_images);
}

void ChatSession::regenerate_from(const std::string& user_text) {
    // 检查是否正在生成
    if (m_generating.load()) {
        m_event_bus.get().publish_async(
            StreamErrorEvent{.session_id = m_session_id,
                             .message = "Still generating, cannot regenerate",
                             .retryable = true});
        return;
    }

    std::vector<std::string> last_user_images;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        // 从后往前找最后一条匹配的用户消息，删除该用户消息及其后所有消息
        // （run_completion 会重新 push 该用户消息，避免重复）
        for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
            if (it->role == ChatMessage::Role::User && it->content == user_text) {
                last_user_images = it->image_paths;
                m_messages.erase(it.base() - 1, m_messages.end());
                break;
            }
        }
    }
    run_completion(user_text, last_user_images);
}

void ChatSession::send_message(const std::string& text, const std::vector<std::string>& images) {
    if (m_generating.load()) {
        // 模型忙碌：进入待发送队列（不丢消息；Ctrl+Enter 由 TUI 先行 request_flush）
        enqueue_message(text, images);
        return;
    }

    run_completion(text, images);
}

// ============================================================
// 消息队列（模型忙碌时缓存用户输入，工具轮边界/整轮结束冲刷）
// ============================================================

bool ChatSession::enqueue_message(const std::string& text, const std::vector<std::string>& images) {
    // 模型空闲时入队无意义：调用方（TUI send_input）应走 send_message 直接发送。
    // 返回 false 让调用方回退到直接发送路径。
    if (!m_generating.load()) return false;

    QueuedMessageItem item;
    item.id = core::util::generate_uuid();
    item.text = text;
    item.images = images;
    item.queued_at_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_pending_queue.push_back(std::move(item));
    }
    publish_queue_update();
    return true;
}

void ChatSession::request_flush() {
    // 冲刷请求：ReActLoop 在下一个工具轮边界调用 inject_pending_queue 时消费。
    // 置位不持锁（原子），避免与后台线程持锁顺序冲突。
    m_flush_requested.store(true);
}

void ChatSession::remove_queued_message(const std::string& id) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        for (auto it = m_pending_queue.begin(); it != m_pending_queue.end(); ++it) {
            if (it->id == id) {
                m_pending_queue.erase(it);
                removed = true;
                break;
            }
        }
    }
    if (removed) publish_queue_update();
}

void ChatSession::clear_pending_queue() {
    bool had_items = false;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        had_items = !m_pending_queue.empty();
        m_pending_queue.clear();
        m_flush_requested.store(false);
    }
    if (had_items) publish_queue_update();
}

std::vector<QueuedMessageItem> ChatSession::queued_messages() const {
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    return {m_pending_queue.begin(), m_pending_queue.end()};
}

void ChatSession::publish_queue_update() {
    m_event_bus.get().publish_async(MessageQueueUpdatedEvent{
        .session_id = m_session_id,
        .items = queued_messages(),
    });
}

void ChatSession::inject_pending_queue(std::vector<ChatMessage>& messages) {
    // 仅在显式请求冲刷（Ctrl+Enter）时在工具轮边界注入；
    // 普通 Enter 入队的消息由 flush_pending_after_run 在整轮结束统一冲刷。
    if (!m_flush_requested.load()) return;

    std::vector<QueuedMessageItem> items;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_pending_queue.empty()) return;
        items.assign(m_pending_queue.begin(), m_pending_queue.end());
        m_pending_queue.clear();
        m_flush_requested.store(false);
    }
    const std::string merged = merge_queued_text(items);
    messages.push_back(ChatMessage::user(merged));
    publish_queue_update();
    // 通知 UI 在转录区回显该合并 user 消息（Ctrl+Enter 工具轮边界冲刷）
    m_event_bus.get().publish_async(QueuedMessagesFlushedEvent{
        .session_id = m_session_id,
        .merged_text = merged,
    });
}

void ChatSession::flush_pending_after_run() {
    std::vector<QueuedMessageItem> items;
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_pending_queue.empty() || m_generating.load()) return;
        items.assign(m_pending_queue.begin(), m_pending_queue.end());
        m_pending_queue.clear();
        m_flush_requested.store(false);
    }
    publish_queue_update();
    // 通知 UI 在转录区回显该合并 user 消息 + 置 busy（整轮收尾冲刷开启新一轮）
    m_event_bus.get().publish_async(QueuedMessagesFlushedEvent{
        .session_id = m_session_id,
        .merged_text = merge_queued_text(items),
    });
    // 队列消息合并为单条 user 消息，开启新一轮推理
    run_completion(merge_queued_text(items));
}

std::string ChatSession::merge_queued_text(const std::vector<QueuedMessageItem>& items) {
    std::string merged;
    for (size_t i = 0; i < items.size(); ++i) {
        merged +=
            std::format("[排队消息 {}/{}]\n{}\n━━━━━━━━━━\n", i + 1, items.size(), items[i].text);
    }
    return merged;
}

// #45：会话级权限模式（两态切换 Default ↔ Bypass，Plan 归入工作模式管理）
void ChatSession::set_permission_mode(tool::PermissionMode mode) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_permission_mode = mode;
    // #87：CLI --permission-mode / factory 注入的初始模式也要落盘，
    // 否则 resume 时读不到任何 permission 事件（首轮就走 fail-safe 分支）。
    persist_permission_state_locked();
}

tool::PermissionMode ChatSession::permission_mode() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_permission_mode;
}

/// @brief 权限两态切换（手动审批 ↔ 完全访问）
/// @details 计划模式已从权限循环中独立为工作模式（m_session_mode），
///          故 Shift+Tab 仅在 Default / BypassPermissions 之间循环。
///          计划模式联动由 toggle_session_mode / EnterPlanMode 工具负责。
void ChatSession::toggle_permission_mode() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    // 计划模式下不直接切换权限（由模式切换统一管理，退出计划时恢复）
    if (m_session_mode == tool::SessionMode::Plan) {
        return;
    }
    bool changed = false;
    switch (m_permission_mode) {
        case tool::PermissionMode::Default:
            m_permission_mode = tool::PermissionMode::BypassPermissions;
            changed = true;
            break;
        case tool::PermissionMode::BypassPermissions:
            m_permission_mode = tool::PermissionMode::Default;
            changed = true;
            break;
        case tool::PermissionMode::Plan:
            // 理论不可达（Plan 由模式管理）；保守回退到 Default
            m_permission_mode = tool::PermissionMode::Default;
            changed = true;
            break;
        case tool::PermissionMode::AcceptEdits:
            break;  // 占位模式，不参与循环
    }
    // #87：Shift+Tab 切换后落盘（会话中途改模式是最常见的边界丢失场景）
    if (changed) persist_permission_state_locked();
}

tool::SessionMode ChatSession::session_mode() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_session_mode;
}

void ChatSession::set_session_mode(tool::SessionMode mode) {
    std::string persist_reason;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        // 离开 Plan：恢复进入前的权限模式（与 ExitPlanMode 工具语义一致）
        if (m_session_mode == tool::SessionMode::Plan && mode != tool::SessionMode::Plan) {
            m_permission_mode = m_permission_mode_before_plan;
        }
        // 进入 Plan：记录 before_plan 并强制只读（对齐 EnterPlanMode 工具语义）
        if (mode == tool::SessionMode::Plan && m_session_mode != tool::SessionMode::Plan) {
            m_permission_mode_before_plan = m_permission_mode;
            m_permission_mode = tool::PermissionMode::Plan;
        }
        m_session_mode = mode;
        // 方案 A：模式变化后按目标模式重建系统提示词（工具说明随模式收窄/恢复）
        persist_reason = rebuild_system_prompt_locked();
        // #87：三态落盘（进入/退出 Plan 会联动 permission_mode，必须一并持久化）
        persist_permission_state_locked();
    }
    if (!persist_reason.empty()) persist_system_prompt(persist_reason);
}

void ChatSession::toggle_session_mode() {
    std::string persist_reason;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        switch (m_session_mode) {
            case tool::SessionMode::Standard:
                // 标准 → 极简：纯降级，权限不变
                m_session_mode = tool::SessionMode::Minimal;
                break;
            case tool::SessionMode::Minimal:
                // 极简 → 计划：记录 before_plan（退出计划恢复用）
                m_permission_mode_before_plan = m_permission_mode;
                m_permission_mode = tool::PermissionMode::Plan;
                m_session_mode = tool::SessionMode::Plan;
                break;
            case tool::SessionMode::Plan:
                // 计划 → 标准：退出计划，恢复进入前的权限模式
                m_permission_mode = m_permission_mode_before_plan;
                m_session_mode = tool::SessionMode::Standard;
                break;
        }
        // 方案 A：模式变化后按目标模式重建系统提示词
        persist_reason = rebuild_system_prompt_locked();
        // #87：三态落盘（append-only，resume 时取最后一条）
        persist_permission_state_locked();
    }
    if (!persist_reason.empty()) persist_system_prompt(persist_reason);
}

// ============================================================
// #87：权限/工作模式持久化与恢复
// ============================================================

void ChatSession::persist_permission_state_locked() {
    if (!m_session_store) return;  // 未开会话 / 内存会话：无文件可写，静默跳过
    agent::session::PermissionEvent ev;
    ev.permission_mode = permission_mode_to_string(m_permission_mode);
    ev.session_mode = session_mode_to_string(m_session_mode);
    ev.permission_mode_before_plan = permission_mode_to_string(m_permission_mode_before_plan);
    ev.in_plan = m_in_plan_mode;
    if (!m_session_store->append_permission(ev)) {
        LOG_WARN("#87: persist permission state failed, session={}", m_session_id);
    }
}

ChatSession::PermissionRestoreResult ChatSession::restore_permission_locked(
    const std::optional<agent::session::PermissionEvent>& ev) {
    PermissionRestoreResult r;

    // 分支 1（no_record）：历史会话或从未切换过模式 —— 事件流里没有 permission 事件。
    // 这不是「曾经处于 Plan」，而是「没记录」，绝大多数是旧会话，故回退 Default 而非从严切 Plan
    // （强行切 Plan 会让所有历史会话 resume 后写不了文件，等于用新的静默失败替换旧的）。
    // 但**必须显式告知**：restored=false + reason 交给 TUI 呈现，不静默降级。
    if (!ev.has_value()) {
        m_permission_mode = tool::PermissionMode::Default;
        m_permission_mode_before_plan = tool::PermissionMode::Default;
        m_session_mode = tool::SessionMode::Standard;
        m_in_plan_mode = false;
        r.restored = false;
        r.reason = "no_record";
        r.mode = m_permission_mode;
        LOG_WARN("#87: session={} has no permission record, fallback to default (explicit notice)",
                 m_session_id);
        return r;
    }

    auto pm = permission_mode_from_string(ev->permission_mode);
    auto sm = session_mode_from_string(ev->session_mode);
    // before_plan 缺失/非法只影响「退出 Plan 时恢复到哪个模式」，可容忍 → 取 Default
    auto bpm = permission_mode_from_string(ev->permission_mode_before_plan);

    // 分支 2（invalid_value）：值非法（跨版本/被篡改）——从严回退 Plan，宁可只读。
    if (!pm.has_value() || !sm.has_value()) {
        m_permission_mode = tool::PermissionMode::Plan;
        m_permission_mode_before_plan = bpm.value_or(tool::PermissionMode::Default);
        m_session_mode = tool::SessionMode::Plan;
        m_in_plan_mode = true;
        r.restored = false;
        r.reason = "invalid_value";
        r.mode = m_permission_mode;
        LOG_WARN("#87: session={} has invalid permission record (mode={}, session_mode={}), "
                 "fallback to plan",
                 m_session_id, ev->permission_mode, ev->session_mode);
        return r;
    }

    // 分支 3：正常恢复三态
    m_permission_mode = *pm;
    m_session_mode = *sm;
    m_permission_mode_before_plan = bpm.value_or(tool::PermissionMode::Default);
    m_in_plan_mode = ev->in_plan;
    r.restored = true;
    r.mode = m_permission_mode;
    return r;
}

ChatSession::PermissionRestoreResult ChatSession::last_permission_restore() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_last_permission_restore;
}

std::string ChatSession::rebuild_system_prompt_locked() {
    std::string reason;
    if (!m_system_prompt_builder) return reason;
    std::string rebuilt = m_system_prompt_builder(m_session_mode);
    if (rebuilt == m_system_prompt) return reason;
    m_system_prompt = std::move(rebuilt);
    reason = m_system_prompt_recorded ? "changed" : "initial";
    return reason;
}

std::vector<ChatMessage> ChatSession::get_messages() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_messages;
}

// DS_CACHE M-3：session_cache_stats() 已删除（死代码，TUI 使用 token_stats_model）

void ChatSession::run_completion(const std::string& user_text,
                                 const std::vector<std::string>& images, int retry_attempt) {
    // B.1：拆分后的 run_completion 仅做顶层调度，agent 循环逻辑分发到子方法
    // 子方法返回 AgentStepResult，决定下一步动作（避免 goto 跨变量声明）

    // 仅首次请求时添加用户消息（重试时不重复添加）
    if (retry_attempt == 0) {
        // conditional skills：提取用户消息中的 <file path> 引用 → touch，
        // 匹配激活的 skill 追加为用户消息前缀（已激活过的不重复注入）
        std::string effective_text = user_text;
        if (m_command_registry) {
            // 1. 用户消息 <file path> 引用 → touch 收集器（与工具上报合并）
            extract_file_path_touches(user_text, m_touch_collector);
            const auto touched = m_touch_collector.paths();
            // 2. 匹配激活的 skill 追加为用户消息前缀（已激活过的不重复注入）
            auto activated = skill::activate_conditional_skills(
                touched, m_command_registry->get_by_type("prompt"), m_cwd);
            // 3. agent 过滤：声明了 agent 且与当前 agent 不符 → 跳过激活
            const auto active_agent =
                m_config_manager.get().get_or<std::string>(agent::keys::AGENT_ACTIVE, "");
            std::string prefix;
            for (const auto& sk : activated) {
                if (m_activated_skills.contains(sk->name())) continue;
                if (sk->agent().has_value() &&
                    (active_agent.empty() || sk->agent().value() != active_agent))
                    continue;
                command::CommandContext sctx;
                sctx.cwd = m_cwd;
                sctx.session_id = m_session_id;
                const auto blocks = sk->generate_prompt("", sctx);
                std::string body;
                for (const auto& b : blocks) {
                    body += b.text;
                    body += "\n";
                }
                // PreActivate hooks：激活时执行，输出并入激活块
                if (!sk->hooks().empty()) {
                    const auto hook_lines = skill::run_preactivate_hooks(sk->hooks(), m_cwd);
                    const auto hook_block = skill::format_hook_output(hook_lines);
                    if (!hook_block.empty()) {
                        body = "[skill hooks]\n" + hook_block + "\n" + body;
                    }
                }
                // 对象式通用 Hook：激活时注册到会话级 HookManager（会话期间生效）
                if (m_hooks && !m_hooks->empty() && !sk->hooks_json().empty()) {
                    for (const auto& json_str : sk->hooks_json()) {
                        try {
                            auto arr = nlohmann::json::parse(json_str);
                            if (!arr.is_array()) continue;
                            for (const auto& obj : arr) {
                                hook::HookDefinition def = hook::HookDefinition::from_json(obj);
                                if (def.command.empty() && def.url.empty() && def.prompt.empty()) {
                                    LOG_WARN(
                                        "[hook] frontmatter hook def missing command/url/prompt, "
                                        "skipped: {}",
                                        obj.dump());
                                    continue;
                                }
                                m_hooks->register_hook(std::move(def));
                            }
                        } catch (const std::exception& e) {
                            LOG_WARN("[hook] invalid frontmatter hooks JSON, skipped: {}",
                                     e.what());
                        }
                    }
                }
                prefix += "[Activated skill: " + sk->name() + "]\n" + body + "\n";
                m_activated_skills.insert(sk->name());
            }
            if (!prefix.empty()) {
                effective_text = prefix + user_text;
            }
        }
        ChatMessage user_msg = images.empty() ? ChatMessage::user(effective_text)
                                              : ChatMessage::user(effective_text, images);
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_messages.push_back(user_msg);
        }
        // 项目会话恢复：用户消息实时持久化
        persist_message(user_msg);
    }

    m_event_bus.get().publish_async(BackendStatusEvent{
        .status = BackendStatusEvent::Connecting, .backend_name = "session", .error = {}});

    m_generating.store(true);

    // H-3：拷贝 HttpRetryPolicy 到任务闭包（不可变结构体，线程安全）
    HttpRetryPolicy retry_policy = m_retry_policy;

    auto task = m_task_manager.get().launch(
        "completion",
        [this, retry_attempt, retry_policy, user_text](const std::atomic<bool>& should_cancel) {
            // 顶层异常安全网：确保任何未捕获异常都能重置 m_generating 并通知 UI
            try {
                // ---- 构建 tools_schema ----
                nlohmann::json tools_schema = nlohmann::json::array();
                if (m_tool_registry && m_tool_registry->size() > 0) {
                    // 极简模式：仅暴露 Skill/Bash/Read/Write/Edit 五个工具
                    // （其余工具对 LLM 不可见，配合 ToolExecutor 守卫双重保障）
                    if (session_mode() == tool::SessionMode::Minimal) {
                        tools_schema = m_tool_registry->get_schemas_by_names(
                            {tool::kMinimalModeToolNames,
                             tool::kMinimalModeToolNames + tool::kMinimalModeToolCount});
                    } else {
                        tools_schema = m_tool_registry->get_all_schemas();
                    }
                }

                // ---- 0.6.x：QueryEngine 统一编排入口（验收标准：唯一 loop 入口）----
                // 按 agent.active 解析 AgentType 构造 ReAct/GoalGuarded，注入同一套依赖 +
                // 会话级权限状态 + 查询追踪调用链。普通对话（agent.active 空）走 ReAct，
                // 行为与以往手动 ReActLoop 完全一致（回归零差异）。
                const auto active_agent_conf =
                    m_config_manager.get().get_or<std::string>(agent::keys::AGENT_ACTIVE, "");
                const std::string goal_spec =
                    m_config_manager.get().get_or<std::string>(agent::keys::AGENT_GOAL, "");

                // 依赖与旧 ReActLoop 手动构造同源（provider/registry/config_manager/
                // task_manager/cwd/compactor/event_bus/touch 注入路径一一对应）。
                GoalAgentDeps gdeps{
                    .provider = m_provider.get(),
                    .registry = m_tool_registry,
                    .config_manager = &m_config_manager.get(),
                    .task_manager = &m_task_manager.get(),
                    .cwd = m_cwd,
                    .external_compactor = &m_compactor,
                    .event_bus = &m_event_bus.get(),
                    .touch_collector = &m_touch_collector,
                    .file_index_invalidator = m_file_index_invalidator,
                    .session_id = m_session_id,
                    // 消息队列：模型忙碌时前端入队的用户消息，在 ReAct 工具轮边界
                    // 合并为单条 user 消息注入循环（Ctrl+Enter 显式冲刷请求）。
                    // 本 lambda 运行于任务线程，this 生命周期由 ChatSession 任务管理保证。
                    .queue_inject_cb =
                        [this](std::vector<ChatMessage>& messages) {
                            inject_pending_queue(messages);
                        },
                    // #56 方案 C：命令注册表 → ToolContext（AgentTool skill 预加载来源）
                    .command_registry = m_command_registry,
                    // #56 方案 D：父会话全局 MCP 管理器（AgentTool 子 Agent mcpServers
                    // 引用复用来源）
                    .mcp_manager = m_mcp_manager,
                };
                QueryEngine query_engine(std::move(gdeps));

                // #45#28：注入会话级权限状态（Default/Plan/Bypass 三态 + Plan 退出恢复），
                // 由 QueryEngine 落到 ReAct 循环与 GoalGuarded 内部循环（后者此前缺失）。
                // 回调写回 ChatSession（受 m_state_mutex 保护）统一状态源，对齐
                // EnterPlanMode/ExitPlanModeV2/on_permission_mode_changed（H-1 PR #46 评审）。
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    query_engine.set_permission(PermissionSnapshot{
                        .mode = m_permission_mode,
                        .before_plan = m_permission_mode_before_plan,
                        .on_changed =
                            [this](tool::PermissionMode mode, tool::PermissionMode before_plan,
                                   bool in_plan) {
                                std::lock_guard<std::mutex> lk(m_state_mutex);
                                m_permission_mode = mode;
                                m_permission_mode_before_plan = before_plan;
                                m_in_plan_mode = in_plan;
                                // 工具路径（EnterPlanMode/ExitPlanModeV2）同步工作模式：
                                // 进入计划 → 模式=计划；退出计划 → 回落到标准模式
                                if (in_plan) {
                                    m_session_mode = tool::SessionMode::Plan;
                                } else if (m_session_mode == tool::SessionMode::Plan) {
                                    m_session_mode = tool::SessionMode::Standard;
                                }
                                // #87：工具路径的 Plan 进出是最关键的只读边界，
                                // 不落盘则「agent 自己进入 Plan → 中断 → resume」后边界静默消失。
                                persist_permission_state_locked();
                            },
                    });
                    // 注入会话工作模式（标准/计划/极简）：极简模式白名单守卫依据
                    query_engine.set_session_mode(m_session_mode);
                }

                // 3.2：使用 IReActObserver 接口替代 lambda 回调
                // ReActEventPublisher 内部完成 ReActStep → IEventBus 事件转换
                ReActEventPublisher publisher(m_event_bus, m_session_id);

                // ---- DS_CACHE: 捕获前缀形状（用于本轮结束后的缓存劣化归因）----
                // H-2：cur_shape 在 run() 后二次捕获，以传入压缩器 rewrite_version。
                //      prev_shape 从上一轮 m_last_prefix_shape 读取（含上轮的 rewrite_version）。
                PrefixShape prev_shape;
                size_t messages_before_loop =
                    0;  // 项目会话恢复：记录 loop 前消息数，用于批量持久化
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    prev_shape = m_last_prefix_shape;
                    messages_before_loop = m_messages.size();
                }
                // 预捕获仅用于 prev_shape 为空（首轮）时的 prefix_hash 基线；
                // 真正的 cur_shape 在 run() 返回后用 react_result.rewrite_version 二次捕获
                PrefixShape cur_shape_baseline = capture_shape(m_system_prompt, tools_schema, 0);
                if (prev_shape.prefix_hash.empty()) {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_last_prefix_shape = cur_shape_baseline;
                }

                // ---- 执行 Agent 循环（QueryEngine 按 agent.active 路由，唯一入口）----
                // 目标守卫 goal 来自 agent.goal；goal_spec 原文透传用于 AgentDoneEvent/
                // AgentVerdictEvent 展示。observer 用 ReActEventPublisher 发布步骤事件。
                AgentRunContext run_ctx{
                    .messages = &m_messages,
                    .system_prompt = m_system_prompt,
                    .tools_schema = tools_schema,
                    .should_cancel = &should_cancel,
                    .goal = parse_goal(goal_spec),
                    .goal_spec = goal_spec,
                    .observer = &publisher,
                };
                AgentRunResult run_result =
                    query_engine.run(m_config_manager.get(), std::move(run_ctx));
                ReActResult react_result = std::move(run_result.react);

                // 记录本会话已分发的后台任务（P1-1 定向取消）：切会话/清除/析构时精确取消
                if (!run_result.background_task_id.empty()) {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_background_task_ids.push_back(run_result.background_task_id);
                }

                // 思考时长回填：reasoning_ms（本 turn 所有 Thought
                // 阶段实际耗时）仅在流式结束后可知， 持久化前回填到本轮最后一条 assistant
                // 消息（写入 JSONL 的 reasoningMs 字段）
                if (react_result.reasoning_ms > 0.0) {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    for (auto it = m_messages.rbegin(); it != m_messages.rend(); ++it) {
                        if (it->role == ChatMessage::Role::Assistant) {
                            it->reasoning_ms = react_result.reasoning_ms;
                            break;
                        }
                    }
                }

                // 项目会话恢复：批量持久化 ReActLoop 新增的 assistant/tool 消息
                persist_messages_range(messages_before_loop);

                // H-2：用 react_result.rewrite_version 二次捕获 cur_shape，使 log_rewrite 归因生效
                PrefixShape cur_shape =
                    capture_shape(m_system_prompt, tools_schema, react_result.rewrite_version);
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_last_prefix_shape = cur_shape;
                }

                // ============================================================
                // 结果处理
                // ============================================================

                // ---- 用户中断 ----
                if (react_result.was_interrupted) {
                    if (!react_result.partial_content.empty()) {
                        ChatMessage partial_msg =
                            ChatMessage::assistant(react_result.partial_content);
                        if (!react_result.partial_reasoning.empty()) {
                            partial_msg.reasoning_content = react_result.partial_reasoning;
                        }
                        {
                            std::lock_guard<std::mutex> lock(m_state_mutex);
                            m_messages.push_back(partial_msg);
                        }
                        // 项目会话恢复：持久化中断时的 partial 消息
                        persist_message(partial_msg);
                    }
                    m_event_bus.get().publish_async(StreamDoneEvent{
                        .session_id = m_session_id,
                        .full_content = react_result.partial_content,
                        .full_reasoning = react_result.partial_reasoning,
                        .was_interrupted = true,
                        .prompt_tokens = react_result.prompt_tokens,
                        .generated_tokens = react_result.generated_tokens,
                        .cache_creation_input_tokens = react_result.cache_creation_input_tokens,
                        .cache_read_input_tokens = react_result.cache_read_input_tokens,
                        .prompt_cache_hit_tokens = react_result.prompt_cache_hit_tokens,
                        .prompt_cache_miss_tokens = react_result.prompt_cache_miss_tokens,
                        .prompt_ms = react_result.prompt_ms,
                        .generation_ms = react_result.generation_ms,
                        .reasoning_ms = react_result.reasoning_ms});
                    m_generating.store(false);
                    flush_pending_after_run();  // 队列收尾冲刷（中断也算本轮结束）
                    return;
                }

                // ---- 错误处理 ----
                // H-7：纯函数 compute_retry 决策，I/O 由 run_completion 执行
                if (react_result.was_error) {
                    auto decision = compute_retry(react_result, retry_policy, retry_attempt);

                    switch (decision.action) {
                        case RetryAction::Sleep: {
                            // 可重试：发布重试提示事件 + 可中断等待 + 递归重试
                            m_event_bus.get().publish_async(StreamErrorEvent{
                                .session_id = m_session_id,
                                .message =
                                    std::format("Error: {}, retrying in {}ms... ({}/{})",
                                                react_result.error_message, decision.delay_ms,
                                                retry_attempt + 1, retry_policy.max_retries),
                                .retryable = true});

                            // 可中断的等待
                            auto wait_until = std::chrono::steady_clock::now() +
                                              std::chrono::milliseconds(decision.delay_ms);
                            while (std::chrono::steady_clock::now() < wait_until) {
                                if (should_cancel) {
                                    m_generating.store(false);
                                    flush_pending_after_run();  // 等待期被打断：本轮终止，收尾冲刷
                                    return;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                            }

                            if (!should_cancel) {
                                // 保留工具调用上下文：不删除已成功的 tool call/result 消息
                                // ReAct loop 在流式失败时不会添加 partial assistant 消息，
                                // 所以 m_messages 中只有成功的 round，直接重试即可
                                run_completion(user_text, {}, retry_attempt + 1);
                                // 递归重试已接管 m_generating（保持 true），本任务退出；
                                // 队列由重试任务的终止路径冲刷，此处不重复冲刷。
                                return;
                            }
                            break;
                        }
                        case RetryAction::Stop:
                            // 不可重试：发布终止事件
                            m_event_bus.get().publish_async(
                                StreamErrorEvent{.session_id = m_session_id,
                                                 .message = react_result.error_message,
                                                 .retryable = false});
                            break;
                        case RetryAction::Continue:
                            // 无错误路径，理论上不应进入（was_error=true 时不会返回 Continue）
                            break;
                    }
                    m_generating.store(false);
                    flush_pending_after_run();  // 不可重试错误终止：收尾冲刷
                    return;
                }

                // ---- 成功完成 ----
                m_event_bus.get().publish_async(StreamDoneEvent{
                    .session_id = m_session_id,
                    .full_content = react_result.final_answer,
                    .full_reasoning = react_result.final_reasoning,
                    .was_interrupted = false,
                    .prompt_tokens = react_result.prompt_tokens,
                    .generated_tokens = react_result.generated_tokens,
                    .cache_creation_input_tokens = react_result.cache_creation_input_tokens,
                    .cache_read_input_tokens = react_result.cache_read_input_tokens,
                    .prompt_cache_hit_tokens = react_result.prompt_cache_hit_tokens,
                    .prompt_cache_miss_tokens = react_result.prompt_cache_miss_tokens,
                    .prompt_ms = react_result.prompt_ms,
                    .generation_ms = react_result.generation_ms,
                    .reasoning_ms = react_result.reasoning_ms});

                // DS_CACHE M-3：移除 m_cache_hit_total/m_cache_miss_total 累加（死代码已删除）
                // TUI 通过 token_stats_model 的 update_from_usage 自行累计

                // DS_CACHE: 发布缓存诊断事件（前缀变化归因）
                // 仅当前缀变化或本轮有 cache 数据时发布，避免无意义事件刷屏
                if (!prev_shape.prefix_hash.empty() || react_result.prompt_cache_hit_tokens > 0 ||
                    react_result.prompt_cache_miss_tokens > 0) {
                    auto diag =
                        compare_shape(prev_shape, cur_shape, react_result.prompt_cache_hit_tokens,
                                      react_result.prompt_cache_miss_tokens);
                    if (diag.prefix_changed || diag.cache_miss_tokens > 0) {
                        m_event_bus.get().publish_async(
                            CacheDiagnosticsEvent{.session_id = m_session_id,
                                                  .prefix_hash = diag.prefix_hash,
                                                  .prefix_changed = diag.prefix_changed,
                                                  .reasons = diag.reasons,
                                                  .cache_hit_tokens = diag.cache_hit_tokens,
                                                  .cache_miss_tokens = diag.cache_miss_tokens});
                    }
                }

                m_event_bus.get().publish_async(AgentDoneEvent{
                    .final_response = react_result.final_answer,
                    .total_steps = static_cast<int32_t>(react_result.steps.size()),
                    .total_tool_calls = react_result.total_tool_calls,
                    .total_duration_ms = react_result.total_duration_ms,
                    // 0.6.x：透传实际 Agent 类型与目标守卫终态（QueryTracker 调用链溯源）
                    .agent_type = static_cast<int32_t>(run_result.agent_type),
                    .goal_status = static_cast<int32_t>(react_result.goal_status),
                    .goal_spec = goal_spec,
                });

                m_generating.store(false);
                flush_pending_after_run();  // 成功完成：队列仍有未发消息则开启新一轮

            }  // end try
            catch (const std::exception& e) {
                m_event_bus.get().publish_async(StreamErrorEvent{
                    .session_id = m_session_id,
                    .message = std::format("Fatal error in completion task: {}", e.what()),
                    .retryable = false});
                m_generating.store(false);
                flush_pending_after_run();
            } catch (...) {
                m_event_bus.get().publish_async(
                    StreamErrorEvent{.session_id = m_session_id,
                                     .message = "Fatal unknown error in completion task",
                                     .retryable = false});
                m_generating.store(false);
                flush_pending_after_run();
            }
        },
        TaskType::Normal);

    // 跟踪当前后台任务，用于析构等待
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_current_task = task;
    }
}

// ============================================================
// 会话持久化
// ============================================================

// ============================================================
// H-7：compute_retry 纯函数（重试决策）
// ============================================================

RetryDecision ChatSession::compute_retry(const ReActResult& react_result,
                                         const HttpRetryPolicy& retry_policy, int attempt) {
    // 无错误：继续执行（非错误路径）
    if (!react_result.was_error) {
        return RetryDecision{RetryAction::Continue, 0};
    }

    // 可重试判定：①未超 max_retries ②HttpRetryPolicy.is_retryable 通过
    // http_status=0 表示业务错误（非 HTTP），由 error_message 内容判断
    const bool can_retry = attempt < retry_policy.max_retries &&
                           HttpRetryPolicy::is_retryable(0, react_result.error_message);

    if (!can_retry) {
        return RetryDecision{RetryAction::Stop, 0};
    }

    // 退避延迟计算（委托 HttpRetryPolicy.delay_ms，含 60s 上限）
    return RetryDecision{RetryAction::Sleep, retry_policy.delay_ms(attempt)};
}

// ============================================================
// H-6：序列化/反序列化纯函数
// ============================================================

nlohmann::json ChatSession::serialize_state() const {
    nlohmann::json j;

    // 拷贝 system_prompt 和 messages，避免长时间持锁
    std::string system_prompt_copy;
    std::vector<ChatMessage> messages_copy;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        system_prompt_copy = m_system_prompt;
        messages_copy = m_messages;
    }

    if (!system_prompt_copy.empty()) {
        j["system_prompt"] = system_prompt_copy;
    }

    // H-6 修复：显式初始化 messages 为数组，避免空消息时 j["messages"] 为 null
    // （原代码 auto& messages = j["messages"]; 在空消息时遗留 null 值）
    nlohmann::json messages = nlohmann::json::array();
    for (const auto& msg : messages_copy) {
        nlohmann::json m;
        switch (msg.role) {
            case ChatMessage::Role::System:
                m["role"] = "system";
                break;
            case ChatMessage::Role::User:
                m["role"] = "user";
                break;
            case ChatMessage::Role::Assistant:
                m["role"] = "assistant";
                break;
            case ChatMessage::Role::Tool:
                m["role"] = "tool";
                break;
        }
        m["content"] = msg.content;
        if (!msg.reasoning_content.empty()) {
            m["reasoning_content"] = msg.reasoning_content;
        }
        if (msg.role == ChatMessage::Role::Tool) {
            m["tool_call_id"] = msg.tool_call_id;
            m["tool_name"] = msg.tool_name;
            if (msg.is_error) {
                m["is_error"] = true;
            }
        }
        // Task 9：序列化 assistant 消息的 tool_uses（保证 /save /load 后不丢失）
        if (msg.role == ChatMessage::Role::Assistant && !msg.tool_uses.empty()) {
            nlohmann::json uses = nlohmann::json::array();
            for (const auto& tu : msg.tool_uses) {
                uses.push_back({{"id", tu.id}, {"name", tu.name}, {"input", tu.input}});
            }
            m["tool_uses"] = std::move(uses);
        }
        messages.push_back(m);
    }
    j["messages"] = std::move(messages);

    return j;
}

// C-1：改为真正纯函数——不修改任何成员状态，仅解析 j 返回 (messages, system_prompt)
Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>
ChatSession::deserialize_state(const nlohmann::json& j) {
    std::vector<ChatMessage> new_messages;
    std::string new_system_prompt;

    try {
        if (j.contains("system_prompt")) {
            new_system_prompt = j["system_prompt"].get<std::string>();
        }

        if (j.contains("messages")) {
            for (const auto& m : j["messages"]) {
                ChatMessage msg;

                // H-6：const operator[] 在 missing key 上触发 assert，必须先 contains 检查
                if (!m.contains("role")) {
                    return Result<std::pair<std::vector<ChatMessage>, std::string>,
                                  std::string>::err("Message missing 'role' field");
                }
                if (!m.contains("content")) {
                    return Result<std::pair<std::vector<ChatMessage>, std::string>,
                                  std::string>::err("Message missing 'content' field");
                }

                std::string role_str = m["role"].get<std::string>();
                if (role_str == "system")
                    msg.role = ChatMessage::Role::System;
                else if (role_str == "user")
                    msg.role = ChatMessage::Role::User;
                else if (role_str == "assistant")
                    msg.role = ChatMessage::Role::Assistant;
                else if (role_str == "tool")
                    msg.role = ChatMessage::Role::Tool;
                else {
                    return Result<std::pair<std::vector<ChatMessage>, std::string>,
                                  std::string>::err(std::format("Unknown role: {}", role_str));
                }

                msg.content = m["content"].get<std::string>();
                if (m.contains("reasoning_content")) {
                    msg.reasoning_content = m["reasoning_content"].get<std::string>();
                }
                if (msg.role == ChatMessage::Role::Tool) {
                    if (m.contains("tool_call_id")) {
                        msg.tool_call_id = m["tool_call_id"].get<std::string>();
                    }
                    if (m.contains("tool_name")) {
                        msg.tool_name = m["tool_name"].get<std::string>();
                    }
                    if (m.contains("is_error")) {
                        msg.is_error = m["is_error"].get<bool>();
                    }
                }
                // Task 9：反序列化 assistant 消息的 tool_uses
                if (msg.role == ChatMessage::Role::Assistant && m.contains("tool_uses")) {
                    for (const auto& tu : m["tool_uses"]) {
                        ToolUse use;
                        use.id = tu.value("id", "");
                        use.name = tu.value("name", "");
                        use.input = tu.value("input", nlohmann::json::object());
                        msg.tool_uses.push_back(std::move(use));
                    }
                }
                new_messages.push_back(std::move(msg));
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
            std::format("JSON parse error: {}", e.what()));
    } catch (const nlohmann::json::type_error& e) {
        return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
            std::format("JSON type error: {}", e.what()));
    } catch (const std::exception& e) {
        return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
            std::format("Error deserializing session: {}", e.what()));
    }

    return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::ok(
        std::make_pair(std::move(new_messages), std::move(new_system_prompt)));
}

// C-1：一次性加锁提交状态到成员
void ChatSession::commit_state(std::vector<ChatMessage> messages, std::string system_prompt) {
    bool record_resume = !system_prompt.empty();
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_messages = std::move(messages);
        m_system_prompt = std::move(system_prompt);
    }
    // 会话轨迹调试：恢复的提示词落盘一条 resume 快照（store 已配置时）
    if (record_resume) {
        persist_system_prompt("resume");
    }
}

// ============================================================
// H-6：save/load 仅做 I/O，序列化逻辑委托纯函数
// ============================================================

Result<void, std::string> ChatSession::save_session(const std::string& path) const {
    try {
        nlohmann::json j = serialize_state();

        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(std::format("Failed to create file: {}", path));
        }
        file << j.dump(2);
        file.close();
        return Result<void, std::string>::ok();

    } catch (const std::exception& e) {
        return Result<void, std::string>::err(std::format("Error saving session: {}", e.what()));
    }
}

Result<void, std::string> ChatSession::load_session(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return Result<void, std::string>::err(std::format("File not found: {}", path));
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(std::format("Failed to open file: {}", path));
        }

        nlohmann::json j;
        file >> j;
        file.close();

        // C-1：deserialize_state 是纯函数，需通过 commit_state 提交到成员
        auto parse_result = deserialize_state(j);
        if (parse_result.isErr()) {
            return Result<void, std::string>::err(parse_result.error());
        }
        auto [messages, system_prompt] = std::move(parse_result).unwrap();
        commit_state(std::move(messages), std::move(system_prompt));
        return Result<void, std::string>::ok();

    } catch (const nlohmann::json::parse_error& e) {
        return Result<void, std::string>::err(std::format("JSON parse error: {}", e.what()));
    } catch (const std::exception& e) {
        return Result<void, std::string>::err(std::format("Error loading session: {}", e.what()));
    }
}

// H-8：backend() 方法已删除，UI 层通过 factory 注入的 IBackendAdmin* 调用管理接口。
// 详见 factory.cpp 中 SessionResult.backend_admin 字段。

// ============================================================
// 事件订阅
// ============================================================

void ChatSession::subscribe_interrupt() {
    m_interrupt_token =
        m_event_bus.get().subscribe<InterruptEvent>([this](const InterruptEvent& /*e*/) {
            // 1) 快速断开 LLM 流（保留原有路径）
            if (m_provider) {
                m_provider->interrupt();
            }
            // 2) 置位当前任务的 should_cancel（#23 P1：运行中中断也要打通
            //    协作取消链路，使 ReActLoop 与工具能即时感知，而非只断流）。
            //    task->cancel() 只置原子标志，线程安全，可在任意线程调用。
            std::shared_ptr<Task> task;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                task = m_current_task;
            }
            if (task) {
                task->cancel();
            }
        });
}

void ChatSession::unsubscribe_interrupt() {
    m_event_bus.get().unsubscribe<InterruptEvent>(m_interrupt_token);
}

void ChatSession::subscribe_sub_agent_persistence() {
    m_sub_progress_token =
        m_event_bus.get().subscribe<SubAgentProgressEvent>([this](const SubAgentProgressEvent& e) {
            // final 步由 SubAgentCompletedEvent 承载，UI 不渲染，跳过持久化
            if (e.step_type == "final") return;
            agent::session::SubAgentEvent ev;
            ev.type = "progress";
            ev.task_id = e.task_id;
            ev.step_number = e.step_number;
            ev.step_type = e.step_type;
            ev.content = e.content;
            ev.thought_text = e.thought_text;
            ev.tool_name = e.tool_name;
            ev.tool_input = e.tool_input;
            ev.observation = e.observation;
            ev.is_error = e.is_error;
            ev.duration_ms = e.duration_ms;
            std::shared_ptr<agent::session::SessionStore> store;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                store = m_session_store;
            }
            if (store) store->append_sub_agent(ev);
        });
    m_sub_completed_token = m_event_bus.get().subscribe<SubAgentCompletedEvent>(
        [this](const SubAgentCompletedEvent& e) {
            agent::session::SubAgentEvent ev;
            ev.type = "completed";
            ev.task_id = e.task_id;
            ev.final_answer = e.final_answer;
            ev.was_error = e.was_error;
            ev.duration_ms = e.duration_ms;
            std::shared_ptr<agent::session::SessionStore> store;
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                store = m_session_store;
            }
            if (store) store->append_sub_agent(ev);
        });
}

void ChatSession::unsubscribe_sub_agent_persistence() {
    m_event_bus.get().unsubscribe<SubAgentProgressEvent>(m_sub_progress_token);
    m_event_bus.get().unsubscribe<SubAgentCompletedEvent>(m_sub_completed_token);
}

// ============================================================
// #54：Plan Mode V2 事件驱动（五阶段 + 并行 explore 编排）
// ============================================================

void ChatSession::subscribe_plan_events() {
    if (!m_plan_coordinator) return;

    // #54：接线真实并行 explore 子 Agent——复用 AgentTool 的子 Agent 启动链路（launch_sub_agent）。
    // 每个 explore 的 task_id 即子 Agent 任务名；完成后发布的 SubAgentCompletedEvent
    // 由下方 m_plan_sub_completed_token 回投 m_plan_coordinator->on_explore_task_done。
    // 权限模式取 Plan（只读）：launch_sub_agent 内会对非只读工具过滤，杜绝写/执行能力。
    m_plan_coordinator->set_explore_runner(
        [this](const std::string& task_id, const std::string& /*area*/, const std::string& prompt) {
            tool::SubAgentLaunchOptions opts;
            opts.task_id = task_id;
            opts.prompt = prompt;
            opts.provider = m_provider.get();
            opts.sub_registry = m_tool_registry;
            opts.command_registry = m_command_registry.get();
            opts.config_manager = &m_config_manager.get();
            opts.task_manager = &m_task_manager.get();
            opts.event_bus = &m_event_bus.get();
            opts.cwd = m_cwd;
            opts.session_id = m_session_id;
            opts.permission_mode = tool::PermissionMode::Plan;  // 只读探索，杜绝写/执行
            opts.hook_manager = m_hooks;
            tool::launch_sub_agent(opts);
        });

    // 进入 Plan（EnterPlanMode 事件）→ 启动 interview/探索流程
    m_plan_enter_token =
        m_event_bus.get().subscribe<EnterPlanModeEvent>([this](const EnterPlanModeEvent& e) {
            if (!m_plan_coordinator) return;
            m_plan_coordinator->begin_plan(e.reason);
            // interview 开启路径：收集的约束以进入原因代填，随后自动推进探索。
            // （交互式 Interview 面板可由宿主另行扩展；当前架构支持在
            //   PlanCoordinator.set_interview_notes 注入真实收集结果。）
            if (m_plan_coordinator->stage() == plan::PlanStage::Interview) {
                m_plan_coordinator->set_interview_notes(e.reason);
            }
            // 探索完成（机械 fallback 合成时同步到 AwaitingApproval）：落盘方案文件
            maybe_persist_plan_artifact();
        });

    // 退出 Plan（ExitPlanModeV2 工具批准/驳回）→ 推进阶段终态
    m_plan_exit_token =
        m_event_bus.get().subscribe<ExitPlanModeEvent>([this](const ExitPlanModeEvent& e) {
            if (!m_plan_coordinator) return;
            m_plan_coordinator->set_approved(e.approved);
            // #54：审批时消费结构化 critical_files（工具缺省 → 产物已聚合；工具显式传入 →
            //       并入产物使执行阶段程序化读取关键文件）
            if (e.approved && !e.critical_files.empty()) {
                m_plan_coordinator->set_critical_files(e.critical_files);
            }
        });

    // 子 Agent 完成 → 回报 explore 发现（按 task_id 匹配）
    m_plan_sub_completed_token = m_event_bus.get().subscribe<SubAgentCompletedEvent>(
        [this](const SubAgentCompletedEvent& e) {
            if (!m_plan_coordinator) return;
            m_plan_coordinator->on_explore_task_done(e.task_id, e.final_answer, e.was_error);
            maybe_persist_plan_artifact();
        });
}

void ChatSession::unsubscribe_plan_events() {
    m_event_bus.get().unsubscribe<EnterPlanModeEvent>(m_plan_enter_token);
    m_event_bus.get().unsubscribe<ExitPlanModeEvent>(m_plan_exit_token);
    m_event_bus.get().unsubscribe<SubAgentCompletedEvent>(m_plan_sub_completed_token);
}

/// @brief 计划产物就绪（AwaitingApproval，含 mechanical fallback）时落盘方案 markdown
/// @details 复写 ~/.workx/plan/plan_<session>.md，供 ExitPlanModeV2 / 侧边栏预览。
void ChatSession::maybe_persist_plan_artifact() {
    if (!m_plan_coordinator) return;
    const auto& art = m_plan_coordinator->artifact();
    if (m_plan_coordinator->stage() != plan::PlanStage::AwaitingApproval || art.markdown.empty()) {
        return;
    }
    namespace fs = std::filesystem;
    try {
        fs::path dir = fs::path(tool::expand_home("~/.workx/plan"));
        fs::create_directories(dir);
        fs::path file = dir / ("plan_" + m_session_id + ".md");
        std::ofstream ofs(file, std::ios::trunc | std::ios::binary);
        if (ofs) ofs << art.markdown;
    } catch (const std::exception&) {
        // 落盘失败静默（不阻断退出确认流程）
    }
}

}  // namespace agent
