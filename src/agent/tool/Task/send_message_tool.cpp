/**
 * @file send_message_tool.cpp
 * @brief SendMessageTool 实现
 * @version 1.0.0
 * @date 2026-10
 */

#include "agent/tool/Task/send_message_tool.h"

#include <format>

#include "core/task/task_manager.h"
#include "core/utils/error.h"

namespace agent::tool {

namespace {

/// @brief 任务状态 → 小写字符串（与 TaskStopTool 同型；TaskStatus 无现成 to_string）
std::string_view task_status_string(TaskStatus s) noexcept {
    switch (s) {
        case TaskStatus::Pending:
            return "pending";
        case TaskStatus::Running:
            return "running";
        case TaskStatus::Completed:
            return "completed";
        case TaskStatus::Cancelled:
            return "cancelled";
        case TaskStatus::Failed:
            return "failed";
    }
    return "unknown";
}

}  // namespace

const std::string& SendMessageTool::name() const {
    static const std::string n{"SendMessage"};
    return n;
}

const std::string& SendMessageTool::description() const {
    static const std::string d{
        "Sends a follow-up instruction to a running background task by task_id."};
    return d;
}

const std::string& SendMessageTool::prompt() const {
    static const std::string p{
        "Sends a follow-up instruction to a running background task (e.g. a sub-agent "
        "launched by Agent). The message is queued on the task and delivered to it at its "
        "next tool-round boundary. Requires the task_id returned by Agent. Fails if the "
        "task does not exist, has finished, or is a system-critical task."};
    return p;
}

nlohmann::json SendMessageTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"task_id",
               {{"type", "string"}, {"description", "The ID of the target background task"}}},
              {"message",
               {{"type", "string"},
                {"description", "The follow-up instruction text to deliver"}}}}},
            {"required", {"task_id", "message"}},
            {"additionalProperties", false}};
}

ResultV2<ToolResult> SendMessageTool::call(const nlohmann::json& input,
                                           const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "SendMessage: input must be an object");
    }
    const std::string task_id = input.value("task_id", "");
    const std::string message = input.value("message", "");
    if (task_id.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "SendMessage: 'task_id' is required");
    }
    if (message.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "SendMessage: 'message' is required");
    }
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::NotImplemented,
                                         "SendMessage: no task manager available");
    }

    auto task = ctx.task_manager_ptr->find_task(task_id);
    if (task == nullptr) {
        // #52 错误分类：not_found —— 目标不存在
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound,
            std::format("[not_found] No task found with ID: {}", task_id));
    }
    // #52 错误分类：unsupported_type —— Critical 系统任务不接受 LLM 投递的指令
    if (task->getType() == TaskType::Critical) {
        return ResultV2<ToolResult>::err(
            Error::Code::UnsupportedTaskType,
            std::format("[unsupported_type] Task {} has type 'critical' and cannot receive "
                        "messages via SendMessage",
                        task_id));
    }
    if (task->isFinished()) {
        // #52 错误分类：not_running —— 目标已结束，消息无处投递
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidTaskState,
            std::format("[not_running] Task {} is not running (status: {}). "
                        "Use TaskOutput to read its result.",
                        task_id, task_status_string(task->getStatus())));
    }

    task->push_pending_message(message);
    return ResultV2<ToolResult>::ok(
        ToolResult::ok(std::format("Message queued for delivery to task: {}", task_id)));
}

}  // namespace agent::tool
