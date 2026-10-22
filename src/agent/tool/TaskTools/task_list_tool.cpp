/**
 * @file task_list_tool.cpp
 * @brief TaskListTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/TaskTools/task_list_tool.h"

#include <format>

#include "agent/tool/TodoStore/todo_store.h"
#include "core/todo/todo_item.h"
#include "core/utils/error.h"

namespace agent::tool {

const std::string& TaskListTool::name() const {
    static const std::string n{"TaskList"};
    return n;
}

const std::string& TaskListTool::description() const {
    static const std::string d{"Lists all tasks in the session task list."};
    return d;
}

const std::string& TaskListTool::prompt() const {
    static const std::string p{
        "Lists all tasks in the current session's task list. "
        "Returns an array of task objects (id, subject, description, status, "
        "activeForm, owner, blocks, blockedBy). Returns an empty array if there "
        "are no tasks."};
    return p;
}

nlohmann::json TaskListTool::input_schema() const {
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {}
    })JSON";
    return nlohmann::json::parse(schema_str);
}

ResultV2<ToolResult> TaskListTool::call(const nlohmann::json& /*input*/,
                                        const ToolContext& ctx) const {
    const auto todos = TodoStore::instance().list_todos(ctx.session_id);
    nlohmann::json result = {{"tasks", todos}};
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
