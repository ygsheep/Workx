#include "bridge/event_bridge.h"

#include <utility>

#include "core/events/agent_events.h"
#include "core/events/stream_events.h"
#include "core/events/system_events.h"

namespace ftxtui {

void EventBridge::push(Action action) {
    m_queue.push(std::move(action));
    if (m_wake) m_wake();
}

void EventBridge::start() {
    subscribe_typed<agent::StreamTokenEvent>([this](const agent::StreamTokenEvent& e) {
        if (!e.content_delta.empty()) push(ActionTokenDelta{e.content_delta});
        if (!e.reasoning_delta.empty()) push(ActionReasoningDelta{e.reasoning_delta});
    });

    subscribe_typed<agent::StreamDoneEvent>([this](const agent::StreamDoneEvent& e) {
        push(ActionTurnDone{
            .full_content = e.full_content,
            .full_reasoning = e.full_reasoning,
            .was_interrupted = e.was_interrupted,
            .is_local_command = e.is_local_command,
            .prompt_tokens = e.prompt_tokens,
            .generated_tokens = e.generated_tokens,
            .cache_read_input_tokens = e.cache_read_input_tokens,
            .prompt_cache_hit_tokens = e.prompt_cache_hit_tokens,
            .prompt_cache_miss_tokens = e.prompt_cache_miss_tokens,
            .prompt_ms = e.prompt_ms,
            .generation_ms = e.generation_ms,
            .reasoning_ms = e.reasoning_ms,
        });
        push(ActionSetBusy{.busy = false});
    });

    subscribe_typed<agent::StepDoneEvent>(
        [this](const agent::StepDoneEvent&) { push(ActionStepDone{}); });

    subscribe_typed<agent::StreamErrorEvent>([this](const agent::StreamErrorEvent& e) {
        push(ActionError{.message = e.message});
        push(ActionSetBusy{.busy = false});
    });

    subscribe_typed<agent::ToolCallEvent>([this](const agent::ToolCallEvent& e) {
        push(ActionBeginTool{
            .tool_name = e.tool_name,
            .call_id = e.call_id,
            .arguments = e.arguments,
        });
    });

    subscribe_typed<agent::ToolResultEvent>([this](const agent::ToolResultEvent& e) {
        push(ActionEndTool{
            .call_id = e.call_id,
            .result = e.result,
            .is_error = e.is_error,
        });
    });

    subscribe_typed<agent::AgentDoneEvent>([this](const agent::AgentDoneEvent& e) {
        push(ActionAgentDone{
            .final_answer = e.final_response,
            .total_steps = e.total_steps,
            .total_tool_calls = e.total_tool_calls,
            .total_duration_ms = e.total_duration_ms,
        });
    });

    subscribe_typed<agent::AskUserRequestEvent>([this](const agent::AskUserRequestEvent& e) {
        push(ActionAskUser{
            .questions = e.questions,
            .timeout_ms = e.timeout_ms,
            .result_promise = e.result_promise,
            .cancel_flag = e.cancel_flag,
        });
    });

    subscribe_typed<agent::AskUserTimeoutEvent>(
        [this](const agent::AskUserTimeoutEvent&) { push(ActionAskUserTimeout{}); });

    // 计划模式已从权限循环中独立为工作模式：进入/退出计划同步模式位
    // （权限位由 ChatSession on_changed 回调统一回写，此处只动模式）
    subscribe_typed<agent::EnterPlanModeEvent>(
        [this](const agent::EnterPlanModeEvent&) { push(ActionSetMode{.label = "plan"}); });

    subscribe_typed<agent::ExitPlanModeEvent>(
        [this](const agent::ExitPlanModeEvent&) { push(ActionSetMode{.label = "standard"}); });

    // 方案预览：退出规划模式时在侧边栏打开方案 markdown 文件
    subscribe_typed<agent::PlanPreviewEvent>([this](const agent::PlanPreviewEvent& e) {
        if (!e.plan_path.empty()) push(ActionOpenPlan{e.plan_path});
    });

    // B3：缓存诊断 / 压缩暂停 / 子 Agent 进度（对齐设计文档 §5 事件映射）
    subscribe_typed<agent::CacheDiagnosticsEvent>([this](const agent::CacheDiagnosticsEvent& e) {
        push(ActionCacheDiagnostics{
            .prefix_hash = e.prefix_hash,
            .prefix_changed = e.prefix_changed,
            .reasons = e.reasons,
            .cache_hit_tokens = e.cache_hit_tokens,
            .cache_miss_tokens = e.cache_miss_tokens,
        });
    });

    subscribe_typed<agent::CompactionPausedEvent>([this](const agent::CompactionPausedEvent& e) {
        push(ActionCompactionPaused{
            .paused = e.paused,
            .consecutive_compacts = e.consecutive_compacts,
            .notice = e.notice,
        });
    });

    // 消息队列更新（模型忙碌时前端入队的用户消息）→ 队列卡片重绘
    subscribe_typed<agent::MessageQueueUpdatedEvent>(
        [this](const agent::MessageQueueUpdatedEvent& e) {
            std::vector<QueueItemLite> items;
            items.reserve(e.items.size());
            for (const auto& it : e.items) {
                items.push_back(QueueItemLite{
                    .id = it.id,
                    .text = it.text,
                    .queued_at_ms = it.queued_at_ms,
                });
            }
            push(ActionQueueUpdate{.session_id = e.session_id, .items = std::move(items)});
        });

    // 队列冲刷：排队消息合并为真实 user 消息注入 ReAct 循环
    // （运行中的 ChatSession 内部完成，绕过 send_input，UI 不会自行回显）。
    // 先回显 user 消息、再置 busy，为新一轮流式回复建立上下文，否则该条 user
    // 消息只停留在队列卡片、冲刷后消失，新一轮回复成为无前置消息的孤儿节点。
    subscribe_typed<agent::QueuedMessagesFlushedEvent>(
        [this](const agent::QueuedMessagesFlushedEvent& e) {
            push(ActionAppendMessage{.role = "user", .text = e.merged_text});
            push(ActionSetBusy{.busy = true});
        });

    subscribe_typed<agent::SubAgentProgressEvent>([this](const agent::SubAgentProgressEvent& e) {
        push(ActionSubAgentProgress{
            .task_id = e.task_id,
            .step_number = e.step_number,
            .step_type = e.step_type,
            .content = e.content,
            // v1.3.0 结构化字段透传（第二层卡片渲染）
            .thought_text = e.thought_text,
            .tool_name = e.tool_name,
            .tool_input = e.tool_input,
            .observation = e.observation,
            .is_error = e.is_error,
            .duration_ms = e.duration_ms,
        });
    });

    subscribe_typed<agent::SubAgentCompletedEvent>([this](const agent::SubAgentCompletedEvent& e) {
        push(ActionSubAgentCompleted{
            .task_id = e.task_id,
            .final_answer = e.final_answer,
            .was_error = e.was_error,
            .duration_ms = e.duration_ms,
        });
    });

    // #24：待办清单更新（TodoStore → 侧边栏 TODO 区块 / StatusBar 进度）
    subscribe_typed<agent::TodoUpdatedEvent>([this](const agent::TodoUpdatedEvent& e) {
        push(ActionTodoUpdate{
            .session_id = e.session_id,
            .todos = e.todos,
        });
    });

    // #27 M4：MCP 后台连接状态变化 → 侧栏状态点刷新
    subscribe_typed<agent::McpStatusChangedEvent>([this](const agent::McpStatusChangedEvent& e) {
        std::vector<McpServerLite> servers;
        servers.reserve(e.servers.size());
        for (const auto& s : e.servers) {
            servers.push_back(McpServerLite{
                s.name,
                s.protocol,
                s.tool_count,
                s.state,
                s.error,
            });
        }
        push(ActionMcpStatus{std::move(servers)});
    });

    // Issue #50 M-2：Hook 执行进度（start/done/failed）→ 输入区上方进度条
    subscribe_typed<agent::HookProgressEvent>([this](const agent::HookProgressEvent& e) {
        push(ActionHookProgress{
            .hook_id = e.hook_id,
            .event = e.event,
            .phase = e.phase,
            .hook_type = e.hook_type,
            .tool_name = e.tool_name,
            .message = e.message,
            .hook_label = e.hook_label,
        });
    });

    subscribe_typed<agent::ShutdownEvent>(
        [this](const agent::ShutdownEvent&) { push(ActionShutdown{}); });
}

void EventBridge::stop() {
    // B4：逐个精确退订（按订阅时登记的 token + 类型），不依赖外部 bus.clear() 兜底，
    //      也不误伤 EventBus 上其他订阅者。
    for (auto& unsub : m_unsubscribers) {
        if (unsub) unsub();
    }
    m_unsubscribers.clear();
}

}  // namespace ftxtui
