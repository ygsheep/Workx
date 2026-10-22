/**
 * @file task_update_tool.cpp
 * @brief TaskUpdateTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/TaskTools/task_update_tool.h"

#include <format>

#include "agent/tool/TodoStore/todo_store.h"
#include "core/todo/todo_item.h"
#include "core/utils/error.h"

namespace agent::tool {

const std::string& TaskUpdateTool::name() const {
    static const std::string n{"TaskUpdate"};
    return n;
}

const std::string& TaskUpdateTool::description() const {
    static const std::string d{
        "Updates a task in the session task list by id. Fields not provided are left "
        "unchanged. Setting status to 'deleted' removes the task."};
    return d;
}

const std::string& TaskUpdateTool::prompt() const {
    static const std::string p{
        "Updates an existing task in the current session's task list by its id. "
        "Only the fields you provide are changed; omitted fields keep their current "
        "value. Returns the updated task object.\n"
        "- taskId: id of the task to update (required)\n"
        "- subject: new title (imperative)\n"
        "- description: new description\n"
        "- activeForm: new present continuous form\n"
        "- status: 'pending' | 'in_progress' | 'completed' | 'deleted' "
        "(deleted removes the task)\n"
        "- owner: new owner\n"
        "- metadata: replace the metadata object\n"
        "- blocks: replace the list of blocked task ids\n"
        "- blockedBy: replace the list of blocking task ids"};
    return p;
}

nlohmann::json TaskUpdateTool::input_schema() const {
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {
            "taskId": {
                "type": "string",
                "description": "The ID of the task to update"
            },
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
            "status": {
                "type": "string",
                "enum": ["pending", "in_progress", "completed", "deleted"],
                "description": "Task status; 'deleted' removes the task"
            },
            "owner": {
                "type": "string",
                "description": "Task owner"
            },
            "metadata": {
                "type": "object",
                "description": "Arbitrary metadata to attach to the task"
            },
            "blocks": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Ids of tasks this task blocks"
            },
            "blockedBy": {
                "type": "array",
                "items": {"type": "string"},
                "description": "Ids of tasks that block this task"
            }
        },
        "required": ["taskId"]
    })JSON";
    return nlohmann::json::parse(schema_str);
}

ResultV2<ToolResult> TaskUpdateTool::call(const nlohmann::json& input,
                                          const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskUpdate: input must be an object");
    }
    const std::string task_id = input.value("taskId", std::string{});
    if (task_id.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "TaskUpdate: 'taskId' is required");
    }

    // 先做 JSON 类型校验：LLM 可能传错类型（如 status 为 number），
    // 避免在 TodoStore::update_todo 的锁内 mutate 中抛 type_error → 部分更新 + 事件丢失
    for (const char* key : {"subject", "description", "activeForm", "status", "owner"}) {
        if (input.contains(key) && !input[key].is_string()) {
            return ResultV2<ToolResult>::err(
                Error::Code::InvalidInput,
                std::string("TaskUpdate: '") + key + "' must be a string");
        }
    }
    if (input.contains("metadata") && !input["metadata"].is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TaskUpdate: 'metadata' must be an object");
    }
    for (const char* key : {"blocks", "blockedBy"}) {
        if (input.contains(key)) {
            if (!input[key].is_array()) {
                return ResultV2<ToolResult>::err(
                    Error::Code::InvalidInput,
                    std::string("TaskUpdate: '") + key + "' must be an array of strings");
            }
            for (const auto& e : input[key]) {
                if (!e.is_string()) {
                    return ResultV2<ToolResult>::err(
                        Error::Code::InvalidInput,
                        std::string("TaskUpdate: '") + key + "' must be an array of strings");
                }
            }
        }
    }

    auto& store = TodoStore::instance();

    // status=deleted → 删除任务（对齐 cc TaskUpdateTool 语义）
    if (input.contains("status")) {
        const std::string status = input["status"].get<std::string>();
        if (status == "deleted") {
            if (!store.delete_todo(ctx.session_id, task_id)) {
                return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                                 "TaskUpdate: task '" + task_id + "' not found");
            }
            nlohmann::json result = {{"deleted", true}};
            return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
        }
    }

    bool updated = store.update_todo(ctx.session_id, task_id, [&](core::todo::TodoItem& item) {
        if (input.contains("subject")) {
            item.content = input["subject"].get<std::string>();
        }
        if (input.contains("description")) {
            item.description = input["description"].get<std::string>();
        }
        if (input.contains("activeForm")) {
            item.active_form = input["activeForm"].get<std::string>();
        }
        if (input.contains("status")) {
            item.status = core::todo::TodoItem::status_from(input["status"].get<std::string>());
        }
        if (input.contains("owner")) {
            item.owner = input["owner"].get<std::string>();
        }
        if (input.contains("metadata") && input["metadata"].is_object()) {
            item.metadata = input["metadata"];
        }
        if (input.contains("blocks") && input["blocks"].is_array()) {
            item.blocks = input["blocks"].get<std::vector<std::string>>();
        }
        if (input.contains("blockedBy") && input["blockedBy"].is_array()) {
            item.blocked_by = input["blockedBy"].get<std::vector<std::string>>();
        }
    });

    if (!updated) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         "TaskUpdate: task '" + task_id + "' not found");
    }

    auto item = store.get_todo(ctx.session_id, task_id);
    nlohmann::json result = {{"task", item ? *item : core::todo::TodoItem{}}};
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
