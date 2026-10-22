/**
 * @file task_get_tool.cpp
 * @brief TaskGetTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/TaskTools/task_get_tool.h"

#include <format>

#include "agent/tool/TodoStore/todo_store.h"
#include "core/utils/error.h"

namespace agent::tool {

const std::string& TaskGetTool::name() const {
    static const std::string n{"TaskGet"};
    return n;
}

const std::string& TaskGetTool::description() const {
    static const std::string d{
        "Gets a single task by id from the session task list. Returns null if not found."};
    return d;
}

const std::string& TaskGetTool::prompt() const {
    static const std::string p{
        "Gets a single task by its id from the current session's task list. "
        "Returns the task object (id, subject, description, status, owner, blockedBy, "
        "blocks) or null if the task does not exist."};
    return p;
}

nlohmann::json TaskGetTool::input_schema() const {
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {
            "taskId": {
                "type": "string",
                "description": "The ID of the task to get"
            }
        },
        "required": ["taskId"]
    })JSON";
    return nlohmann::json::parse(schema_str);
}

ResultV2<ToolResult> TaskGetTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskGet: input must be an object");
    }
    const std::string task_id = input.value("taskId", std::string{});
    if (task_id.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "TaskGet: 'taskId' is required");
    }

    auto item = TodoStore::instance().get_todo(ctx.session_id, task_id);
    if (!item) {
        // 对齐 cc：任务不存在返回 null（非错误，模型可自行处理）
        nlohmann::json result = nlohmann::json::object();
        result["task"] = nullptr;
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
    }

    nlohmann::json result = {
        {"task", *item},
    };
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
