/**
 * @file task_create_tool.cpp
 * @brief TaskCreateTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/TaskTools/task_create_tool.h"

#include <format>

#include "agent/tool/TodoStore/todo_store.h"
#include "core/todo/todo_item.h"
#include "core/utils/error.h"

namespace agent::tool {

const std::string& TaskCreateTool::name() const {
    static const std::string n{"TaskCreate"};
    return n;
}

const std::string& TaskCreateTool::description() const {
    static const std::string d{
        "Creates a new task in the session task list. Returns the assigned task id. "
        "Use for fine-grained task management (V2); for a simple full-list update use TodoWrite."};
    return d;
}

const std::string& TaskCreateTool::prompt() const {
    static const std::string p{
        "Creates a new task in the current session's task list. "
        "Returns the auto-assigned numeric task id, which you must keep for later "
        "TaskGet / TaskUpdate calls.\n"
        "- subject: brief title for the task (imperative)\n"
        "- description: what needs to be done\n"
        "- activeForm: present continuous form shown while in_progress "
        "(e.g. \"Running tests\")\n"
        "- metadata: arbitrary key-value metadata (optional)"};
    return p;
}

nlohmann::json TaskCreateTool::input_schema() const {
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {
            "subject": {
                "type": "string",
                "description": "A brief title for the task"
            },
            "description": {
                "type": "string",
                "description": "What needs to be done"
            },
            "activeForm": {
                "type": "string",
                "description": "Present continuous form shown in spinner when in_progress (e.g. 'Running tests')"
            },
            "metadata": {
                "type": "object",
                "description": "Arbitrary metadata to attach to the task"
            }
        },
        "required": ["subject", "description"]
    })JSON";
    return nlohmann::json::parse(schema_str);
}

ResultV2<ToolResult> TaskCreateTool::call(const nlohmann::json& input,
                                          const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskCreate: input must be an object");
    }
    // 先做 JSON 类型校验（LLM 可能传错类型，避免 value()/get<>() 抛 type_error）
    for (const char* key : {"subject", "description", "activeForm"}) {
        if (input.contains(key) && !input[key].is_string()) {
            return ResultV2<ToolResult>::err(
                Error::Code::InvalidInput,
                std::string("TaskCreate: '") + key + "' must be a string");
        }
    }
    if (input.contains("metadata") && !input["metadata"].is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskCreate: 'metadata' must be an object");
    }
    const std::string subject = input.value("subject", std::string{});
    if (subject.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "TaskCreate: 'subject' is required");
    }

    core::todo::TodoItem item;
    item.content = subject;
    item.description = input.value("description", std::string{});
    item.active_form = input.value("activeForm", std::string{});
    item.status = core::todo::TodoStatus::Pending;
    if (input.contains("metadata") && input["metadata"].is_object()) {
        item.metadata = input["metadata"];
    }

    const std::string id = TodoStore::instance().create_todo(ctx.session_id, item);

    nlohmann::json result = {
        {"task", {{"id", id}, {"subject", subject}}},
    };
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
