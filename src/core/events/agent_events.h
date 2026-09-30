/**
 * @file agent_events.h
 * @brief Agent 编排事件类型（H-10：从 events.h 按域拆分）
 * @details Agent 推理步骤、工具调用、工具结果、Agent 编排完成等事件。
 *          订阅方按需 include 本文件，避免引入系统/流式事件。
 * @version 1.2.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <future>
#include <memory>
#include <atomic>
#include <nlohmann/json.hpp>

#include "core/tool_kind.h"  // C-3：直接引用 core 层规范位置，避免 core→agent 分层越界
#include "core/todo/todo_item.h"  // #24：待办清单条目（TodoStore → UI 事件载荷）

namespace agent {

// ============================================================
// AskUser 宿主交互结果（宿主无关，TUI 等宿主自行转换自有类型）
// ============================================================

/// @brief AskUser 提问结果（宿主无关类型）
/// @details 由 TUI ChoicePanel 等宿主在 set_value 时填回，
///          与 tui::ChoiceResult 同构，避免 core/agent 依赖 UI 层。
struct AskUserResult {
    bool submitted = false;  ///< true=用户提交, false=取消/超时
    /// @brief 答案映射: question → answer（单选/多选/自定义输入）
    std::vector<std::pair<std::string, std::string>> answers;
};

// ============================================================
// Agent 事件（Agent → TUI，未来）
// ============================================================

/// @brief Agent 推理步骤
struct AgentStepEvent {
    std::string step_id;
    int32_t step_number = 0;
    std::string description;
};

/// @brief Agent 调用工具
struct ToolCallEvent {
    std::string tool_name;
    std::string arguments;
    std::string call_id;
    agent::tool::ToolType tool_type = agent::tool::ToolType::Other;
};

/// @brief 工具返回结果
struct ToolResultEvent {
    std::string call_id;
    std::string result;
    bool is_error = false;
};

/// @brief Agent 编排完成
/// @details 0.6.x：新增 agent_type / goal_status / goal_spec，透传本次查询的
///          实际 Agent 类型与目标验证终态，供 UI 展示与 QueryTracker 溯源。
///          字段用 int32_t 存枚举值（core 层不依赖 agent 类型，见 McpServerStatusLite 惯例）。
struct AgentDoneEvent {
    std::string final_response;
    int32_t total_steps = 0;
    int32_t total_tool_calls = 0;
    double total_duration_ms = 0.0;
    int32_t agent_type = 0;   ///< AgentType 枚举值（0=Unknown 1=ReAct 2=GoalGuarded …）
    int32_t goal_status = 0;  ///< GoalStatus 枚举值（0=Unknown 1=Pending 2=Achieved 3=Failed）
    std::string goal_spec;    ///< agent.goal 原文（空 = 普通对话，无目标守卫）
};

/// @brief 目标验证进度事件（GoalGuardedAgent 每轮 check_goal 后发布，0.6.x）
/// @details 供 TUI 第二层/侧栏展示 Verdict 进度（如"第 N 轮验证：测试仍红"）。
///          goal_status 用 int32_t 存 GoalStatus 枚举值，保持 core 层不依赖 agent 类型。
struct AgentVerdictEvent {
    std::string session_id;
    std::string goal_spec;    ///< agent.goal 原文（展示用）
    int32_t attempt = 0;      ///< 当前尝试轮数（1-based）
    int32_t goal_status = 0;  ///< GoalStatus 枚举值（0=Unknown 1=Pending 2=Achieved 3=Failed）
    std::string detail;  ///< 验证器返回的人可读说明（如测试失败数/缺失路径）
};

/// @brief Hook 执行进度事件（Issue #50 M-2：hook 进度可视化）
/// @details 每条 hook 执行开始/结束时经 IEventBus 异步发布，供 UI 展示
///          hook 执行状态（起始/完成/失败）。engine 侧仅负责发布；渲染由
///          订阅方（如 TUI 卡片/状态栏）决定。phase 取 "start" / "done" / "failed"。
struct HookProgressEvent {
    std::string session_id;
    uint64_t hook_id =
        0;  ///< 单次 hook 执行唯一 id（HookManager 单调递增分配，供 start/done 关联）
    std::string event;       ///< 触发事件名（PreToolUse/Stop/...）
    std::string phase;       ///< "start" / "done" / "failed"
    std::string hook_type;   ///< command / http / prompt / agent
    std::string tool_name;   ///< 关联工具名（PreToolUse 等工具事件才有）
    std::string message;     ///< 执行结果摘要（done/failed 时填充）
    std::string hook_label;  ///< 展示标签（command 内容 / prompt 摘要，UI 展示用）
};

/// @brief 缓存诊断事件（DeepSeek 硬盘缓存命中率劣化归因）
/// @details 当某轮缓存命中率显著下降时发布，用于 UI 显示归因。
///          reasons 取值："system" / "tools" / "log_rewrite"
struct CacheDiagnosticsEvent {
    std::string session_id;
    std::string prefix_hash;           ///< 当前前缀形状 hash（system + tools 联合）
    bool prefix_changed = false;       ///< 与上一轮相比前缀是否变化
    std::vector<std::string> reasons;  ///< 变化原因（空表示无变化）
    int32_t cache_hit_tokens = 0;      ///< 本轮命中 token 数
    int32_t cache_miss_tokens = 0;     ///< 本轮未命中 token 数
};

/// @brief 压缩暂停事件（DS_CACHE H-3：卡死守卫触发/恢复）
/// @details 当 CacheAwareCompactor 连续 compact 达阈值时发布（paused=true），
///          当 ratio 回落到 soft 以下自愈恢复时发布（paused=false）。
///          UI 据此提示"压缩已暂停，前缀重新 append-only 增长以恢复缓存命中"。
struct CompactionPausedEvent {
    std::string session_id;
    bool paused = true;                ///< true=卡死守卫触发暂停；false=自愈恢复
    int32_t consecutive_compacts = 0;  ///< 触发时的连续 compact 次数
    int32_t tokens_before = 0;         ///< 触发时的 token 数
    float ratio = 0.0f;                ///< 触发时的窗口占用比
    std::string notice;                ///< 人类可读说明
};

/// @brief AskUser 请求事件（AskUserTool → TUI，触发 ChoicePanel 模态）
/// @details AskUserTool 在工作线程发布本事件后阻塞等待 result_promise；
///          TUI 主循环 drain 异步事件后弹出 ChoicePanel，用户操作完成时
///          调用 result_promise->set_value() 回填结果唤醒工作线程。
///          timeout_ms > 0 时，工作线程等待超时后自动返回 timeout 状态，
///          同时置位 cancel_flag 并发布 AskUserTimeoutEvent 通知 TUI 关闭面板。
struct AskUserRequestEvent {
    std::string session_id;
    nlohmann::json questions;  ///< 原样转交 parse_choice_config 的 JSON
    int32_t timeout_ms = 0;    ///< 超时毫秒（0=不限时；>0 超时自动返回）
    /// @brief 结果回填通道：TUI 设置 value 后唤醒阻塞的 AskUserTool
    /// @details shared_ptr 使事件按值传递时 promise 仍共享同一实例
    std::shared_ptr<std::promise<AskUserResult>> result_promise;
    /// @brief 取消标志：工作线程超时后置位，TUI 主循环检查后关闭 ChoicePanel
    /// @details shared_ptr<atomic<bool>> 使事件按值传递时标志仍共享同一实例。
    ///          nullptr 表示不支持取消（timeout_ms == 0 时可为空）。
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

/// @brief AskUser 超时事件（AskUserTool → TUI，通知主循环关闭 ChoicePanel）
/// @details AskUserTool 在工作线程超时后发布本事件。ChatRenderer 订阅后
///          调用 Terminal::wake_main_loop()，使阻塞在 read_char 的主循环
///          返回 KEY_WAKE，run_choice_panel 检查 cancel_flag 后返回 cancelled。
struct AskUserTimeoutEvent {
    std::string session_id;
};

/// @brief 进入计划模式事件（#28：EnterPlanModeTool → TUI/宿主）
/// @details AI 主动请求进入计划模式（只读调研阶段）。宿主收到后可展示
///          "计划模式"状态；权限模式切换由 ToolContext 回调完成，
///          本事件仅用于 UI 通知。
struct EnterPlanModeEvent {
    std::string session_id;
    std::string reason;  ///< 进入计划模式的原因（AI 说明，可空）
};

/// @brief 退出计划模式事件（#28：ExitPlanModeV2Tool → TUI/宿主）
/// @details AI 完成规划后发布：携带方案文本与用户批准结果。
///          approved=true 表示用户批准方案，可进入执行阶段。
///          #54：critical_files 为结构化关键文件列表（explore 产物聚合，供执行阶段消费）。
struct ExitPlanModeEvent {
    std::string session_id;
    std::string plan;       ///< 方案文本（改哪些文件、风险点等）
    bool approved = false;  ///< 用户是否批准
    std::vector<std::string> critical_files;  ///< #54：关键文件（结构化，可为空由宿主从产物补齐）
};

/// @brief 方案预览事件（ExitPlanModeV2Tool → TUI）
/// @details 退出规划模式呈现方案时，先把方案写入 markdown 文件，再发布本事件，
///          通知 TUI 在侧边栏以 /view 方式打开该文件预览，避免在提问里直接堆全文。
struct PlanPreviewEvent {
    std::string session_id;
    std::string plan_path;  ///< 方案 markdown 文件绝对路径（已落盘，供 TUI 读取）
};

/// @brief 子 Agent 后台任务完成事件（AgentTool → 订阅者，v1.1.0 后台结果自动回送）
/// @details 后台子 Agent 结束后由 AgentTool 发布，携带 task_id 与最终结果摘要，
///          使父会话/UI 等订阅者无需轮询 TaskOutput 即可感知子任务完成。
///          ⚠️ 仅作通知，不注入父 LLM 上下文（避免长输出刷屏父会话，见设计决策）。
///          完整输出仍通过 TaskOutputTool 按 task_id 读取。
struct SubAgentCompletedEvent {
    std::string task_id;  ///< 子 Agent 任务 id（AgentTool 生成的 'a'+8 随机）
    std::string final_answer;  ///< 最终答案（Final: ...）或错误信息（Error: ...），可为空
    bool was_error = false;    ///< 子任务是否以错误结束
    double duration_ms = 0.0;  ///< 子 Agent 循环耗时（毫秒，与 AgentDoneEvent 同型）
};

/// @brief 子 Agent 进度事件（AgentTool → 订阅者，v1.2.0 子任务进度流式订阅）
/// @details 子 Agent 每个 ReAct 步骤完成时增量发布，携带 task_id 与当前步骤信息，
///          使订阅者可按 task_id 实时跟踪子任务进度（无需轮询 TaskOutput）。
///          ⚠️ 仅作增量通知，不注入父 LLM 上下文（与 SubAgentCompletedEvent 同策略）。
///          完整输出仍通过 TaskOutputTool 按 task_id 读取。
///          v1.3.0：补充结构化字段（thought_text/tool_name/tool_input/observation/is_error），
///          供第二层卡片渲染复用主会话 UI（思考卡/工具卡），content 仍保留格式化行。
struct SubAgentProgressEvent {
    std::string task_id;      ///< 子 Agent 任务 id（AgentTool 生成的 'a'+8 随机）
    int32_t step_number = 0;  ///< 当前步骤序号（1-based，与 ReActStep.step_number 对齐）
    std::string step_type;    ///< 步骤类型："thought" / "action" / "observation" / "final"
    std::string content;  ///< 步骤内容（与写入 Task 输出缓冲的格式化行相同，可为空）
    // --- v1.3.0 结构化字段（与 ReActStep 对应）---
    std::string thought_text;  ///< thought/final 的 LLM 文本
    std::string tool_name;     ///< action 的工具名
    std::string tool_input;    ///< action 的工具参数 JSON 字符串
    std::string observation;   ///< observation 的工具结果文本
    bool is_error = false;     ///< 工具执行是否出错
    double duration_ms = 0.0;  ///< 本步骤耗时（毫秒，思考卡标签展示用）
};

/// @brief 后台 Agent 进度事件（BackgroundAgent → 订阅者，长时任务逐步流式订阅）
/// @details 与 SubAgentProgressEvent 同构（字段镜像 ReActStep），供第二层卡片
///          渲染复用主会话 UI。task_id 前缀 'b'，与子 Agent 'a' 区分，避免混淆。
///          ⚠️ 仅作增量通知，不注入主会话 LLM 上下文（与 SubAgent 同策略）。
struct BackgroundProgressEvent {
    std::string task_id;      ///< 后台任务 id（'b'+8 随机）
    int32_t step_number = 0;  ///< 步骤序号（1-based，与 ReActStep.step_number 对齐）
    std::string step_type;    ///< "thought" / "action" / "observation" / "final"
    std::string content;      ///< 步骤内容（格式化行，可为空）
    // --- 结构化字段（与 ReActStep 对应，供第二层卡片渲染复用）---
    std::string thought_text;  ///< thought/final 的 LLM 文本
    std::string tool_name;     ///< action 的工具名
    std::string tool_input;    ///< action 的工具参数 JSON 字符串
    std::string observation;   ///< observation 的工具结果文本
    bool is_error = false;     ///< 工具执行是否出错
    double duration_ms = 0.0;  ///< 本步骤耗时（毫秒）
};

/// @brief 后台 Agent 完成事件（BackgroundAgent → 订阅者，后台结果通知）
/// @details 仅携带 task_id + 结果摘要，不注入主会话 LLM 上下文（防刷屏）。
///          完整输出仍通过 TaskOutput 按 task_id 读取。
struct BackgroundCompletedEvent {
    std::string task_id;  ///< 后台任务 id（'b'+8 随机）
    std::string final_answer;  ///< 最终答案（Final: ...）或错误信息（Error: ...），可为空
    bool was_error = false;    ///< 后台任务是否以错误结束
    double duration_ms = 0.0;  ///< 后台 Agent 循环耗时（毫秒）
};

/// @brief 待办清单更新事件（#24：TodoStore → TUI 侧边栏/StatusBar）
/// @details TodoStore 每次变更（TaskCreate/Update/Delete/List、TodoWrite 全量替换、
///          restore_todos 恢复）后异步发布，携带该 session 完整清单快照。
///          TUI 主循环 drain 后经 ActionTodoUpdate 更新 ViewModel，触发侧边栏重绘。
struct TodoUpdatedEvent {
    std::string session_id;                   ///< 所属会话 id
    std::vector<core::todo::TodoItem> todos;  ///< 变更后的完整清单快照
};

// ============================================================
// MCP server 状态（#27 M4：后台连接结果 → TUI 侧栏）
// ============================================================

/// @brief MCP server 状态条目（轻量，供 UI 侧栏展示；core 层不依赖 agent/mcp 类型）
struct McpServerStatusLite {
    std::string name;      ///< server 名
    std::string protocol;  ///< 协商协议版本（"2026-07-28" / "2025-11-25"）
    int tool_count = 0;    ///< 已预取工具数
    int state = 0;         ///< 0=连接中 1=已连接 2=失败
    std::string error;     ///< 失败原因（state==2 时）
};

/// @brief MCP server 状态变化事件（#27 M4：后台连接完成/失败时发布，UI 刷新侧栏）
/// @details McpClientManager 后台线程逐个连接 server，每完成/失败一个即异步发布
///          全量快照；TUI 主循环 drain 后经 ActionMcpStatus 更新侧栏（彩色状态点）。
struct McpStatusChangedEvent {
    std::vector<McpServerStatusLite> servers;  ///< 全部配置 server 的状态快照
};

// ============================================================
// 消息队列（模型忙碌时缓存用户输入，工具轮边界/整轮结束冲刷）
// ============================================================

/// @brief 排队消息条目（ChatSession 队列元素，TUI 队列卡片展示 + 单条移除）
/// @details 由 ChatSession::enqueue_message 生成，仅存在于内存队列：
///          未发送前不持久化、不进入 m_messages（发送时才合并为 user 消息）。
struct QueuedMessageItem {
    std::string id;                   ///< uuid（单条移除用）
    std::string text;                 ///< 用户文本
    std::vector<std::string> images;  ///< 图片附件绝对路径（可为空）
    int64_t queued_at_ms = 0;         ///< 入队时刻（毫秒时间戳）
};

/// @brief 消息队列更新事件（ChatSession → TUI 队列卡片）
/// @details 入队/移除/冲刷后发布当前队列全量快照（空 = 已清空，卡片消失）。
struct MessageQueueUpdatedEvent {
    std::string session_id;
    std::vector<QueuedMessageItem> items;  ///< 当前队列快照（空=无排队消息）
};

/// @brief 队列冲刷事件（ChatSession → TUI 转录区回显）
/// @details 排队消息被合并为单条 user 消息注入 ReAct 循环（整轮收尾冲刷 / 工具轮
///          边界 Ctrl+Enter 冲刷）时发布，携带合并后的文本。UI 据此在转录区回显
///          该 user 消息并置 busy=true，为新一轮流式回复建立上下文；否则排队消息
///          只停留在队列卡片、冲刷后消失，新一轮回复也成了无前置消息的孤儿节点
///          （仅 /resume 全量重建才可见）。publish 先于新一轮 run_completion，
///          经 FIFO 异步队列保证本事件先于该轮 StreamTokenEvent 被 UI 处理。
struct QueuedMessagesFlushedEvent {
    std::string session_id;
    std::string merged_text;  ///< merge_queued_text() 合并后的 user 消息文本
};

}  // namespace agent
