/**
 * @file react_loop.h
 * @brief ReActLoop — Reason → Act → Observe 迭代循环
 * @details 实现 Thought/Action/Observation 三阶段显式分离的 agent 循环，
 *          替代 ChatSession 中的扁平 while 循环。
 *          使用原生 function calling（Anthropic/OpenAI），不依赖文本解析。
 * @version 1.2.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>

#include "core/export.h"
#include <atomic>
#include <chrono>

#include "agent/api/chat_types.h"
#include "agent/api/i_completion_provider.h"
#include "agent/compact/cache_aware_compactor.h"  // DS_CACHE: 替换死代码 ContextCompressor
#include "agent/tool/registry.h"
#include "agent/tool/executor.h"
#include "agent/tool/context.h"
#include "agent/core/goal_verdict.h"  // GoalStatus（#31：目标导向 Agent 结果）

namespace agent {

// 前向声明（3.2 IReActObserver）
class IReActObserver;
class IConfigManager;  // D-5：前向声明，避免头文件强依赖
class ITaskManager;    // BashTool 后台任务 DI（agent 命名空间下）
class IEventBus;       // AskUserTool 事件发布 DI
namespace skill {
class TouchCollector;
}  // namespace skill
namespace hook {
class HookManager;
}  // namespace hook
namespace command {
class CommandRegistry;
}  // namespace command
namespace mcp {
class McpClientManager;
}  // namespace mcp

// ============================================================
// ReAct 步骤类型
// ============================================================

/// @brief ReAct 步骤类型
enum class ReActStepType {
    Thought,      ///< LLM 推理（流式输出文本 + 工具调用决策）
    Action,       ///< 工具调用执行
    Observation,  ///< 工具结果回传
    FinalAnswer   ///< 最终回复（终止循环）
};

// ============================================================
// ReActStep — 单个步骤记录
// ============================================================

/// @brief 单个 ReAct 步骤记录
///
/// 每个步骤对应循环中的一个阶段（Thought/Action/Observation/FinalAnswer），
/// 用于 UI 展示、调试和日志记录。
struct ReActStep {
    ReActStepType type = ReActStepType::Thought;  ///< 步骤类型
    int step_number = 0;                          ///< 全局步骤序号（从 1 开始）

    // --- Thought 阶段字段 ---
    std::string thought_text;        ///< LLM 文本输出
    std::string reasoning;           ///< LLM 推理内容（thinking）
    std::vector<ToolUse> tool_uses;  ///< LLM 决定调用的工具列表

    // --- Action 阶段字段 ---
    std::string tool_name;      ///< 当前执行的工具名
    std::string tool_use_id;    ///< 工具调用唯一 ID（与 tool_uses[].id 对应）
    nlohmann::json tool_input;  ///< 工具输入参数

    // --- Observation 阶段字段 ---
    std::string observation;  ///< 工具结果文本
    bool is_error = false;    ///< 工具执行是否出错

    // --- 元信息 ---
    double duration_ms = 0.0;  ///< 本步骤耗时（毫秒）
};

// ============================================================
// ReActResult — 循环执行结果
// ============================================================

/// @brief ReAct 循环执行结果
///
/// 由 ReActLoop::run() 返回，包含步骤历史、最终输出和统计信息。
/// ChatSession 根据结果状态决定是否重试、发布完成事件等。
struct ReActResult {
    // --- 步骤历史 ---
    std::vector<ReActStep> steps;  ///< 所有步骤记录

    // --- 最终输出 ---
    std::string final_answer;     ///< LLM 最终回复文本
    std::string final_reasoning;  ///< LLM 最终回复的推理内容

    // --- 统计信息 ---
    int total_iterations = 0;        ///< 总迭代轮数（每轮含 Thought+Action+Observation）
    int total_tool_calls = 0;        ///< 总工具调用次数
    double total_duration_ms = 0.0;  ///< 总耗时（毫秒）

    // --- token 统计（最后一次 LLM 响应）---
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    // 上下文管理：Anthropic cache usage（OpenAI adapter 留 0）
    // 用于精确计算当前上下文 token 总量（命中 cache 时 prompt_tokens 不含 cache 部分）
    int32_t cache_creation_input_tokens = 0;
    int32_t cache_read_input_tokens = 0;
    // 上下文管理：DeepSeek 硬盘缓存命中（Anthropic adapter 留 0）
    int32_t prompt_cache_hit_tokens = 0;   ///< DeepSeek usage.prompt_cache_hit_tokens
    int32_t prompt_cache_miss_tokens = 0;  ///< DeepSeek usage.prompt_cache_miss_tokens
    // DS_CACHE H-2：压缩器改写版本号（run() 结束时回填 m_compactor.rewrite_version()）
    // ChatSession 据此传入 capture_shape，使 compare_shape 的 log_rewrite 归因生效
    int32_t rewrite_version = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
    double reasoning_ms = 0.0;  ///< 思考阶段实际耗时（本 turn 所有 Thought 阶段之和，毫秒）

    // --- 状态 ---
    bool was_interrupted = false;  ///< 用户中断
    bool was_error = false;        ///< 发生错误（流式错误/提交失败/超迭代数）
    std::string error_message;     ///< 错误信息（was_error=true 时有效）
    /// @brief #89：错误对应的 HTTP 状态码（0 = 非 HTTP 错误）
    /// @details 供 compute_retry 判定可重试性，以及后续 fallback 判定是否值得换模型。
    ///          无 HTTP 响应时保持 0（语义与 HttpRetryPolicy 的 http_status=0 一致）。
    int http_status = 0;

    // --- 0.6.x：#31 目标验证状态（普通对话恒为 Unknown）---
    GoalStatus goal_status = GoalStatus::Unknown;

    // --- 部分输出（中断/错误时可能有部分内容）---
    std::string partial_content;    ///< 中断时的部分文本
    std::string partial_reasoning;  ///< 中断时的部分推理
};

// ============================================================
// ReActLoop — ReAct 循环本体
// ============================================================

/// @brief ReActLoop — Reason → Act → Observe 迭代循环
///
/// 从 ChatSession 提取的 agent 循环逻辑，实现显式的三阶段分离：
///
/// @par 循环结构
/// ```
/// for (iteration = 1; iteration <= max_iterations; ++iteration) {
///     // === Thought 阶段 ===
///     // 流式读取 LLM 响应，收集 content + reasoning + tool_uses
///     // 通过 on_token 回调实时推送 token 到 UI
///
///     // 终止判断：无 tool_use → FinalAnswer，退出循环
///
///     // === Action 阶段 ===
///     // 对每个 tool_use，通过 ToolExecutor 执行
///     // 通过 on_token 回调推送 [Tool: name] 到 UI
///
///     // === Observation 阶段 ===
///     // 将 tool_result 消息添加到对话历史
///     // 通过 on_token 回调推送 [Result: ...] 到 UI
/// }
/// ```
///
/// @par 终止条件
/// - LLM 无 tool_use（FinalAnswer）
/// - 达到 max_iterations
/// - 用户取消（should_cancel）
/// - 流式错误
///
/// @par 与 ChatSession 的分工
/// - ReActLoop：负责循环逻辑、工具执行、步骤记录
/// - ChatSession：负责会话管理、重试、持久化、状态标志
class WORKX_API ReActLoop {
   public:
    // ============================================================
    // 配置与回调类型
    // ============================================================

    /// @brief 循环配置
    struct Config {
        /// 基础预算：最大迭代轮数（原硬编码 25，现默认 40，可由 agent.max_iterations 覆盖）。
        /// 预算耗尽或检测到重复工具调用时，内部评审器（review_*）可追加额外预算继续。
        int max_iterations = 40;
        /// 停滞/超限评审开关（内部一次性评审器，不暴露为工具 schema）
        bool review_enabled = true;
        /// 停滞判定窗口：同一 (工具名+规范化输入) 签名在最近 N 次调用内再次出现即视为循环
        int review_stall_window = 4;
        /// 评审"继续"时追加的额外迭代预算（块大小）
        int review_extra_budget = 8;
        /// @brief 达上限评审的"继续"允许次数上限（硬性总预算 = max_iterations + extra*grants）
        int review_max_grants = 2;
        CacheAwareCompactor::Config compactor_cfg;  ///< DS_CACHE: 缓存感知压缩配置

        /// @brief Issue #50：通用 Hook 事件系统（可空；空则全部跳过，零开销）
        /// @details ReActLoop 在 PreToolUse / PostToolUse / Stop 三处调用，
        ///          blockingError/preventContinuation 语义作用于当前 turn。
        std::shared_ptr<hook::HookManager> hooks;
    };

    /// @brief 步骤回调（每完成一个步骤时调用）
    ///
    /// 用于发布 Agent 事件（AgentStepEvent / ToolCallEvent / ToolResultEvent）。
    /// 在 Thought/Action/Observation/FinalAnswer 每个步骤完成时触发。
    using StepCallback = std::function<void(const ReActStep&)>;

    /// @brief 流式 token 回调（仅 Thought 阶段 LLM 输出 delta 时调用）
    ///
    /// 用于发布 StreamTokenEvent。仅在 LLM 流式输出 content/reasoning delta 时触发。
    /// 工具调用/结果反馈通过 StepCallback（Action/Observation 步骤）发布。
    using TokenCallback =
        std::function<void(const std::string& content_delta, const std::string& reasoning_delta)>;

    // ============================================================
    // 构造与析构
    // ============================================================

    /// @brief 构造
    /// @param provider 推理提供者（非拥有，ChatSession 拥有）
    /// @param registry 工具注册表（可为 nullptr，表示无工具）
    /// @param config 循环配置
    /// @param config_manager 配置管理器（H-5：必须非空，注入到 ToolContext）
    /// @param task_manager 任务管理器（可选，用于 BashTool 等后台任务工具）
    /// @param cwd 工作目录（注入到 ToolContext.cwd，空则用进程当前目录）
    /// @param external_compactor 外部压缩器（DS_CACHE H-3：跨 turn 持久化卡死/rewrite 状态，
    ///                            nullptr 则使用内部默认 compactor，状态仅 turn 内有效）
    /// @param event_bus 事件总线（可选，用于 AskUserTool 等需要发布事件的工具）
    /// @param touch_collector 工具 touch 收集器（可选，用于 conditional skills）
    /// @param file_index_invalidator 宿主文件索引失效回调（可选，FileWriteTool 写文件后调用，
    ///                                无宿主时为 nullptr）
    /// @param queue_inject_cb 消息队列冲刷回调（可选，模型忙碌时前端入队的用户消息，
    ///                        在工具轮边界合并为单条 user 消息注入 messages；空 = 无队列）
    ReActLoop(ICompletionProvider* provider, std::shared_ptr<tool::ToolRegistry> registry,
              Config config, IConfigManager* config_manager, ITaskManager* task_manager = nullptr,
              std::string cwd = "", CacheAwareCompactor* external_compactor = nullptr,
              IEventBus* event_bus = nullptr, skill::TouchCollector* touch_collector = nullptr,
              std::function<void()> file_index_invalidator = nullptr, std::string session_id = "",
              std::function<void(std::vector<ChatMessage>&)> queue_inject_cb = {});

    /// @brief 构造（使用默认配置）
    /// @param config_manager 配置管理器（H-5：必须非空，注入到 ToolContext）
    /// @param cwd 工作目录（注入到 ToolContext.cwd，空则用进程当前目录）
    /// @param event_bus 事件总线（可选，用于 AskUserTool 等需要发布事件的工具）
    /// @param session_id 会话 ID（#30：注入到 ToolContext.session_id，供审计日志关联，
    ///                    空则使用默认值 "default"）
    ReActLoop(ICompletionProvider* provider, std::shared_ptr<tool::ToolRegistry> registry,
              IConfigManager* config_manager, ITaskManager* task_manager = nullptr,
              std::string cwd = "", IEventBus* event_bus = nullptr, std::string session_id = "")
        : ReActLoop(provider, std::move(registry), Config{}, config_manager, task_manager,
                    std::move(cwd), nullptr, event_bus, nullptr, nullptr, std::move(session_id)) {}

    ~ReActLoop() = default;

    // 不可拷贝，不可移动（m_provider 引用外部对象，移动后悬空）
    ReActLoop(const ReActLoop&) = delete;
    ReActLoop& operator=(const ReActLoop&) = delete;
    ReActLoop(ReActLoop&&) = delete;
    ReActLoop& operator=(ReActLoop&&) = delete;

    // ============================================================
    // 主接口
    // ============================================================

    /// @brief 执行 ReAct 循环
    ///
    /// @param messages 对话历史（会被修改：追加 assistant + tool_result 消息）
    /// @param system_prompt 系统提示词
    /// @param tools_schema 工具 schema 数组（注入到 CompletionRequest.tools）
    /// @param should_cancel 外部取消信号
    /// @param on_step 步骤回调（可选，默认 nullptr）
    /// @param on_token 流式 token 回调（可选，默认 nullptr）
    /// @return 循环执行结果
    ReActResult run(std::vector<ChatMessage>& messages, const std::string& system_prompt,
                    const nlohmann::json& tools_schema, const std::atomic<bool>& should_cancel,
                    StepCallback on_step = nullptr, TokenCallback on_token = nullptr);

    /// @brief 执行 ReAct 循环（观察者版本，3.2）
    ///
    /// @details 与上述 run() 等价，但通过 IReActObserver 接口发布事件，
    ///          替代 StepCallback + TokenCallback 两个 std::function。
    ///          新代码应优先使用此版本；旧版本保留向后兼容。
    /// @param observer 观察者指针（nullptr 表示无观察者）
    ReActResult run(std::vector<ChatMessage>& messages, const std::string& system_prompt,
                    const nlohmann::json& tools_schema, const std::atomic<bool>& should_cancel,
                    IReActObserver* observer);

    /// @brief DS_CACHE H-3：注入压缩器暂停回调（卡死守卫触发/恢复时通知 ChatSession）
    /// @details ChatSession 据此发布 CompactionPausedEvent 到 EventBus
    void set_compaction_paused_callback(CacheAwareCompactor::PausedCallback cb) {
        m_compactor.set_paused_callback(std::move(cb));
    }

    /// @brief DS_CACHE M-5：重置压缩器状态（clear_history 时调用）
    void reset_compactor() { m_compactor.reset(); }

    /// @brief #28：设置/读取会话级权限模式
    /// @details EnterPlanModeTool/ExitPlanModeV2Tool 通过 ToolContext 回调
    ///          切换该值；构造 ToolContext 时注入当前值。
    /// @note 评审 #2：该模式为 ReActLoop 内存成员，不随会话压缩/恢复或
    ///       /resume 切换持久化，跨这些操作会重置为 Default（Plan 只读边界
    ///       静默丢失）。如需跨会话保留只读状态，需纳入会话恢复流程。
    void set_permission_mode(tool::PermissionMode mode) { m_permission_mode = mode; }
    tool::PermissionMode permission_mode() const { return m_permission_mode; }

    /// @brief #45：注入初始权限状态（跨 turn 恢复会话级三态模式）
    /// @details ChatSession 每次新建 ReActLoop 时调用（ReActLoop 为 run_completion
    ///          局部变量，每轮重建），同步恢复 m_permission_mode、
    ///          m_permission_mode_before_plan 与 m_in_plan_mode，使
    ///          Default/Plan/Bypass 三态与 Plan 退出恢复逻辑跨 turn 保持。
    /// @param mode 当前权限模式（Default/Plan/BypassPermissions）
    /// @param before_plan 进入 Plan 前的原模式（Plan 退出恢复用）
    /// @param in_plan 是否处于 Plan 模式（mode==Plan 时恒为 true）
    void apply_permission_state(tool::PermissionMode mode, tool::PermissionMode before_plan,
                                bool in_plan) {
        m_permission_mode = mode;
        m_permission_mode_before_plan = before_plan;
        m_in_plan_mode = in_plan;
    }

    /// @brief 设置会话工作模式（标准 / 计划 / 极简）
    /// @details 每轮新建 ReActLoop 时由宿主注入（与 apply_permission_state 同路径），
    ///          构造 ToolContext 时携带，ToolExecutor 据此执行极简模式白名单守卫。
    void set_session_mode(tool::SessionMode mode) { m_session_mode = mode; }
    tool::SessionMode session_mode() const { return m_session_mode; }

    /// @brief #56 方案 C：注入命令注册表（bundled + 磁盘技能），供 AgentTool 子 Agent
    ///        skill 预加载取全文；构造 ToolContext 时注入 command_registry_ptr。
    void set_command_registry(std::shared_ptr<command::CommandRegistry> registry) {
        m_command_registry = std::move(registry);
    }

    /// @brief #56 方案 D：注入会话级 MCP 连接管理器，构造 ToolContext 时注入
    ///        mcp_manager_ptr（供工具访问当前作用域可用 MCP server）。
    void set_mcp_manager(std::shared_ptr<agent::mcp::McpClientManager> mgr) {
        m_mcp_manager = std::move(mgr);
    }

    /// @brief H-1（PR #46 评审）：权限状态变更通知回调（宿主 ChatSession 注入）
    /// @details 工具路径回调（on_permission_mode_changed / on_enter_plan_mode /
    ///          on_exit_plan_mode）修改 ReActLoop 投影状态后调用，宿主据此回写
    ///          持久状态（受宿主 m_state_mutex 保护），恢复单一状态源
    ///          （ChatSession），修复"Plan 粘死/丢失"双状态源分裂。
    /// @param mode 当前权限模式
    /// @param before_plan 进入 Plan 前的原模式（Plan 退出恢复用）
    /// @param in_plan 是否处于 Plan 模式
    using PermissionStateChangedCallback = std::function<void(
        tool::PermissionMode mode, tool::PermissionMode before_plan, bool in_plan)>;
    void set_permission_state_changed_callback(PermissionStateChangedCallback cb) {
        m_perm_state_changed_cb = std::move(cb);
    }

   private:
    // ============================================================
    // 内部类型
    // ============================================================

    /// @brief Thought 阶段执行结果
    struct ThoughtResult {
        std::string content;             ///< LLM 文本输出
        std::string reasoning;           ///< LLM 推理内容
        std::vector<ToolUse> tool_uses;  ///< LLM 决定调用的工具
        int32_t prompt_tokens = 0;
        int32_t generated_tokens = 0;
        int32_t cache_creation_input_tokens = 0;  ///< Anthropic cache_creation_input_tokens
        int32_t cache_read_input_tokens = 0;      ///< Anthropic cache_read_input_tokens
        int32_t prompt_cache_hit_tokens = 0;      ///< DeepSeek usage.prompt_cache_hit_tokens
        int32_t prompt_cache_miss_tokens = 0;     ///< DeepSeek usage.prompt_cache_miss_tokens
        double prompt_ms = 0.0;
        double generation_ms = 0.0;

        /// @brief Thought 状态
        enum Status {
            Completed,  ///< 流式正常完成
            Error,      ///< 流式错误
            Cancelled   ///< 用户取消
        } status = Completed;

        /// @brief #89：错误时的 HTTP 状态码（0 = 无 HTTP 响应，如网络错误 / 提交失败）
        /// @details 此前状态码在流层被丢弃，上层只能拿到固定串描述，无法区分
        ///          429（可重试、可降级）与 401（重试无益）。
        int http_status = 0;
        std::string error_message;  ///< 错误详情（status == Error 时有效）
    };

    /// @brief 内部评审器决定（停滞检测 / 达上限时判断"是否继续"）
    /// @details 只喂关键信息（目标、工具序列、最近 observation、预算），
    ///          返回 continue（注入纠偏指令、追加预算）或 wrap_up（优雅收尾）。
    struct ReviewerDecision {
        bool continue_loop = false;  ///< true=注入纠偏继续; false=收尾
        std::string correction;      ///< continue 时注入到 messages 的纠偏指令
        std::string wrap_summary;    ///< wrap_up 时的收尾摘要（为空回退部分进展）
    };

    /// @brief 工具调用签名（停滞检测窗口元素）
    struct ToolCallSignature {
        std::string tool_name;
        std::string normalized_input;
        bool operator==(const ToolCallSignature& o) const {
            return tool_name == o.tool_name && normalized_input == o.normalized_input;
        }
    };

    // ============================================================
    // 内部方法
    // ============================================================

    /// @brief 构建 CompletionRequest
    CompletionRequest build_request(const std::vector<ChatMessage>& messages,
                                    const std::string& system_prompt,
                                    const nlohmann::json& tools_schema) const;

    /// @brief 执行 Thought 阶段（流式读取 LLM 响应）
    ///
    /// 向 provider 提交请求，流式读取响应，收集 content/reasoning/tool_uses。
    /// 每个 content/reasoning delta 通过 on_token 回调推送。
    ///
    /// @param request 推理请求
    /// @param should_cancel 取消信号
    /// @param on_token token 回调
    /// @return Thought 结果
    ThoughtResult execute_thought(const CompletionRequest& request,
                                  const std::atomic<bool>& should_cancel, TokenCallback on_token);

    /// @brief Fallback：从 content 文本中扫描内嵌的 JSON 工具调用
    /// @details 当本地推理后端未走标准 delta.tool_calls 协议时，模型可能在
    ///          content 中输出 JSON 形式的工具调用。本函数扫描 content 中的
    ///          JSON 对象，识别 {"name":"...","arguments":{...}} 或
    ///          {"tool":"...","input":{...}} 形式的块并转为 ToolUse
    /// @param content LLM 文本输出
    /// @param out_tools [out] 解析到的 ToolUse 列表
    static void parse_embedded_tool_calls(const std::string& content,
                                          std::vector<ToolUse>& out_tools);

    /// @brief 规范化工具输入为签名（仅保留键名与标量/数组结构，丢弃顺序与空白）
    /// @param input 工具输入 JSON 对象
    /// @return 规范化签名字符串
    static std::string normalize_tool_input(const nlohmann::json& input);

    /// @brief 内部评审器：一次性 completion 判断"是否继续"
    /// @details 只喂关键信息（用户目标、最近工具执行序列与观察、当前预算），
    ///          返回 ReviewerDecision。失败/解析异常时默认 wrap_up（不冒险继续）。
    /// @param user_request 用户原始请求摘要（截断）
    /// @param tool_history 最近工具执行日志（tool_name: 首行观察）
    /// @param iteration 当前迭代
    /// @param remaining_budget 剩余基础预算（<=0 表示已耗尽/超限）
    /// @param at_limit 是否为达上限评审（true 时追加预算逻辑由调用方处理）
    ReviewerDecision run_reviewer(const std::string& user_request,
                                  const std::vector<std::string>& tool_history, int iteration,
                                  int remaining_budget, bool at_limit) const;

    /// @brief H-1（PR #46 评审）：权限状态变更通知宿主（ChatSession 回写持久状态）
    /// @details 工具路径回调（on_permission_mode_changed / on_enter_plan_mode /
    ///          on_exit_plan_mode）修改投影状态后调用。宿主回调受宿主锁保护，
    ///          此处不持锁调用，避免与宿主持锁上下文嵌套死锁。
    void notify_permission_state() const {
        if (m_perm_state_changed_cb) {
            m_perm_state_changed_cb(m_permission_mode, m_permission_mode_before_plan,
                                    m_in_plan_mode);
        }
    }

    /// @brief 工具轮边界冲刷队列（消息队列：模型忙碌时前端入队的用户消息）
    /// @details 仅在显式请求冲刷（Ctrl+Enter）时注入；普通 Enter 入队的消息
    ///          由 ChatSession::flush_pending_after_run 在整轮结束统一冲刷。
    ///          注入到 messages（下一轮 Thought 一并发送）。空回调 = 无队列。
    /// @param messages 会话历史（会被追加合并后的 user 消息）
    void maybe_inject_queue(std::vector<ChatMessage>& messages) {
        if (m_queue_inject_cb) {
            m_queue_inject_cb(messages);
        }
    }

    // ============================================================
    // 成员
    // ============================================================

    ICompletionProvider* m_provider = nullptr;  ///< 推理提供者（非拥有，外部对象须长于 ReActLoop）
    std::shared_ptr<tool::ToolRegistry> m_registry;  ///< 工具注册表
    std::unique_ptr<tool::ToolExecutor> m_executor;  ///< 工具执行器
    Config m_config;                                 ///< 循环配置
    std::unique_ptr<CacheAwareCompactor>
        m_owned_compactor;             ///< 内部拥有的压缩器（未注入外部时创建）
    CacheAwareCompactor& m_compactor;  ///< DS_CACHE: 压缩器引用（外部注入 or 内部拥有）
    IConfigManager* m_config_manager =
        nullptr;  ///< H-5：配置管理器（非拥有，注入到 ToolContext，必须非空）
    ITaskManager* m_task_manager = nullptr;  ///< BashTool 后台任务 DI（可选，注入到 ToolContext）
    IEventBus* m_event_bus = nullptr;  ///< AskUserTool 事件发布 DI（可选，注入到 ToolContext）
    std::string m_cwd;  ///< 工作目录（会话启动时捕获，注入到 ToolContext.cwd）
    std::string m_session_id;  ///< #30：会话 ID（注入到 ToolContext.session_id，审计日志关联）
    skill::TouchCollector* m_touch_collector =
        nullptr;  ///< conditional skills touch 收集器（可选）
    std::function<void()>
        m_file_index_invalidator;  ///< 宿主文件索引失效回调（可选，注入到 ToolContext）
    tool::PermissionMode m_permission_mode{tool::PermissionMode::Default};  ///< #28：会话级权限模式
    tool::PermissionMode m_permission_mode_before_plan{
        tool::PermissionMode::Default};  ///< #28 评审 #1：进入计划模式前保存的原模式，退出时恢复
    bool m_in_plan_mode{false};  ///< #28 评审 #1/#3：是否处于计划模式（幂等进入判定）
    tool::SessionMode m_session_mode{
        tool::SessionMode::Standard};  ///< 会话工作模式（标准/计划/极简）
    /// @brief H-1（PR #46 评审）：权限状态变更通知回调（宿主 ChatSession 注入，回写持久状态）
    PermissionStateChangedCallback m_perm_state_changed_cb;
    /// @brief 消息队列冲刷回调（可选；工具轮边界把排队用户消息注入 messages）
    std::function<void(std::vector<ChatMessage>&)> m_queue_inject_cb;

    /// @brief #56 方案 C：命令注册表（bundled + 磁盘技能；注入 ToolContext.command_registry_ptr）
    std::shared_ptr<command::CommandRegistry> m_command_registry;
    /// @brief #56 方案 D：会话级 MCP 连接管理器（注入 ToolContext.mcp_manager_ptr）
    std::shared_ptr<agent::mcp::McpClientManager> m_mcp_manager;
};

}  // namespace agent
