/**
 * @file task_output_tool.cpp
 * @brief TaskOutputTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/Task/task_output_tool.h"

#include <format>
#include <chrono>
#include <thread>

#include "core/task/task_manager.h"
#include "core/utils/error.h"

namespace agent::tool {

namespace {

/// @brief 任务状态 → 字符串（对齐 TS TaskStatusSchema 风格）
std::string status_string(TaskStatus status) {
    switch (status) {
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

const std::string& TaskOutputTool::name() const {
    static const std::string n{"TaskOutput"};
    return n;
}

const std::string& TaskOutputTool::description() const {
    static const std::string d{"Reads the output of a background task by task_id."};
    return d;
}

const std::string& TaskOutputTool::prompt() const {
    static const std::string p{
        "Reads the output and status of a background task (e.g. a sub-agent launched by Agent). "
        "Requires the task_id returned by Agent. "
        "With block=true (default) waits up to timeout_ms for the task to finish."};
    return p;
}

nlohmann::json TaskOutputTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {{"task_id", {{"type", "string"}, {"description", "The task ID to get output from"}}},
          {"block",
           {{"type", "boolean"}, {"description", "Whether to wait for completion (default true)"}}},
          {"timeout",
           {{"type", "number"},
            {"description", "Max wait time in ms (default 30000, max 600000)"}}}}},
        {"required", {"task_id"}},
        {"additionalProperties", false}};
}

ResultV2<ToolResult> TaskOutputTool::call(const nlohmann::json& input,
                                          const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskOutput: input must be an object");
    }
    const std::string task_id = input.value("task_id", "");
    if (task_id.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "TaskOutput: 'task_id' is required");
    }
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::NotImplemented,
                                         "TaskOutput: no task manager available");
    }

    auto task = ctx.task_manager_ptr->find_task(task_id);
    if (task == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         std::format("No task found with ID: {}", task_id));
    }

    // 阻塞等待完成（100ms 轮询，对齐 TS 语义）
    const bool block = input.value("block", true);
    int timeout_ms = input.value("timeout", 30000);
    timeout_ms = std::clamp(timeout_ms, 0, 600000);

    bool timed_out = false;
    if (block && !task->isFinished()) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!task->isFinished()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(nlohmann::json{
        {"task_id", task_id},
        {"status", status_string(task->getStatus())},
        {"timed_out", timed_out},
        {"output", task->output()}}.dump()));
}

}  // namespace agent::tool