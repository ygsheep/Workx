/**
 * @file task_stop_tool.cpp
 * @brief TaskStopTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/Task/task_stop_tool.h"

#include <format>

#include "core/task/task_manager.h"
#include "core/utils/error.h"

namespace agent::tool {

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
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         std::format("No task found with ID: {}", task_id));
    }
    if (task->isFinished()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InternalError,
            std::format("Task {} is not running (status: finished)", task_id));
    }

    ctx.task_manager_ptr->cancel(task);
    return ResultV2<ToolResult>::ok(
        ToolResult::ok(std::format("Successfully stopped task: {}", task_id)));
}

}  // namespace agent::tool