/**
 * @file task_stop_tool.cpp
 * @brief TaskStopTool 实现
 * @version 1.1.0
 * @date 2026-08
 */

#include "agent/tool/Task/task_stop_tool.h"

#include <format>

#include "core/task/task_manager.h"
#include "core/utils/error.h"

namespace agent::tool {

namespace {

/// @brief 任务状态 → 小写字符串（用于错误消息；TaskStatus 无现成 to_string）
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

const std::string& TaskStopTool::name() const {
    static const std::string n{"TaskStop"};
    return n;
}

const std::string& TaskStopTool::description() const {
    static const std::string d{"Stops a running background task by task_id."};
    return d;
}

const std::string& TaskStopTool::prompt() const {
    static const std::string p{
        "Stops a running background task (e.g. a sub-agent launched by Agent). "
        "Requires the task_id returned by Agent. Fails if the task does not exist or is not "
        "running."};
    return p;
}

nlohmann::json TaskStopTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"task_id",
               {{"type", "string"}, {"description", "The ID of the background task to stop"}}}}},
            {"required", {"task_id"}},
            {"additionalProperties", false}};
}

ResultV2<ToolResult> TaskStopTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskStop: input must be an object");
    }
    const std::string task_id = input.value("task_id", "");
    if (task_id.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "TaskStop: 'task_id' is required");
    }
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::NotImplemented,
                                         "TaskStop: no task manager available");
    }

    auto task = ctx.task_manager_ptr->find_task(task_id);
    if (task == nullptr) {
        // #52 StopTaskError 分类：not_found —— 目标不存在
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound,
            std::format("[not_found] No task found with ID: {}", task_id));
    }
    // #52 StopTaskError 分类：unsupported_type —— Critical 是系统关键任务，LLM 不可停止
    if (task->getType() == TaskType::Critical) {
        return ResultV2<ToolResult>::err(
            Error::Code::UnsupportedTaskType,
            std::format("[unsupported_type] Task {} has type 'critical' and cannot be "
                        "stopped via TaskStop",
                        task_id));
    }
    if (task->isFinished()) {
        // #52 StopTaskError 分类：not_running —— 目标已结束（可能结果已就绪）
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidTaskState,
            std::format("[not_running] Task {} is not running (status: {}). "
                        "Use TaskOutput to read its result.",
                        task_id, task_status_string(task->getStatus())));
    }

    ctx.task_manager_ptr->cancel(task);
    return ResultV2<ToolResult>::ok(
        ToolResult::ok(std::format("Successfully stopped task: {}", task_id)));
}

}  // namespace agent::tool