/**
 * @file todo_write_tool.cpp
 * @brief TodoWriteTool 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/TodoWriteTool/todo_write_tool.h"

#include <algorithm>
#include <format>

#include "agent/tool/TodoStore/todo_store.h"
#include "core/todo/todo_item.h"
#include "core/utils/error.h"

namespace agent::tool {

const std::string& TodoWriteTool::name() const {
    static const std::string n{"TodoWrite"};
    return n;
}

const std::string& TodoWriteTool::description() const {
    static const std::string d{
        "Update the todo list for the current session. To be used proactively and often "
        "to track progress and pending tasks. Make sure that at least one task is "
        "in_progress at all times. Always provide both content (imperative) and "
        "activeForm (present continuous) for each task."};
    return d;
}

const std::string& TodoWriteTool::prompt() const {
    static const std::string p{
        "Use this tool to create and manage a structured task list for your current "
        "coding session. This helps you track progress, organize complex tasks, and "
        "demonstrate thoroughness to the user.\n"
        "It also helps the user understand the progress of the task and overall "
        "progress of their requests.\n\n"
        "## When to Use This Tool\n"
        "Use this tool proactively in these scenarios:\n"
        "1. Complex multi-step tasks - When a task requires 3 or more distinct steps\n"
        "2. Non-trivial and complex tasks - Tasks that require careful planning\n"
        "3. User explicitly requests todo list\n"
        "4. User provides multiple tasks (numbered or comma-separated)\n"
        "5. After receiving new instructions - Immediately capture requirements as todos\n"
        "6. When you start working on a task - Mark it as in_progress BEFORE beginning\n"
        "7. After completing a task - Mark it as completed and add follow-up tasks\n\n"
        "## When NOT to Use This Tool\n"
        "1. There is only a single, straightforward task\n"
        "2. The task is trivial and tracking provides no organizational benefit\n"
        "3. The task can be completed in less than 3 trivial steps\n"
        "4. The task is purely conversational or informational\n\n"
        "## Task States and Management\n"
        "1. Task States: pending (not started), in_progress (currently working on, "
        "limit to ONE at a time), completed (finished successfully)\n"
        "2. Task descriptions must have two forms:\n"
        "   - content: imperative form (e.g. \"Run tests\")\n"
        "   - activeForm: present continuous form (e.g. \"Running tests\")\n"
        "3. Update task status in real-time; mark complete IMMEDIATELY after finishing\n"
        "4. Exactly ONE task must be in_progress at any time\n"
        "5. Remove tasks that are no longer relevant from the list entirely\n"
        "6. ONLY mark a task as completed when you have FULLY accomplished it; "
        "if blocked, keep it in_progress and create a task describing what to resolve\n\n"
        "When in doubt, use this tool. Being proactive with task management "
        "demonstrates attentiveness and ensures you complete all requirements."};
    return p;
}

nlohmann::json TodoWriteTool::input_schema() const {
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {
            "todos": {
                "type": "array",
                "description": "The updated todo list",
                "items": {
                    "type": "object",
                    "properties": {
                        "content": {
                            "type": "string",
                            "description": "Imperative form of the task, e.g. 'Run tests'"
                        },
                        "status": {
                            "type": "string",
                            "enum": ["pending", "in_progress", "completed"],
                            "description": "Task status"
                        },
                        "activeForm": {
                            "type": "string",
                            "description": "Present continuous form shown while in_progress, e.g. 'Running tests'"
                        }
                    },
                    "required": ["content", "status", "activeForm"]
                }
            }
        },
        "required": ["todos"]
    })JSON";
    return nlohmann::json::parse(schema_str);
}

ResultV2<ToolResult> TodoWriteTool::call(const nlohmann::json& input,
                                         const ToolContext& ctx) const {
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "TodoWrite: input must be an object");
    }
    if (!input.contains("todos") || !input["todos"].is_array()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "TodoWrite: 'todos' array is required");
    }

    std::vector<core::todo::TodoItem> parsed;
    for (const auto& j : input["todos"]) {
        if (!j.is_object()) {
            return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                             "TodoWrite: each todo must be an object");
        }
        // 先做 JSON 类型校验（LLM 可能传错类型，避免 value()/get<>() 抛 type_error）
        for (const char* key : {"content", "activeForm", "status"}) {
            if (j.contains(key) && !j[key].is_string()) {
                return ResultV2<ToolResult>::err(
                    Error::Code::InvalidInput,
                    std::string("TodoWrite: '") + key + "' must be a string");
            }
        }
        core::todo::TodoItem item;
        item.content = j.value("content", std::string{});
        item.active_form = j.value("activeForm", std::string{});
        item.status = core::todo::TodoItem::status_from(j.value("status", std::string{"pending"}));
        if (item.content.empty()) {
            return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                             "TodoWrite: each todo must have non-empty 'content'");
        }
        parsed.push_back(std::move(item));
    }

    auto& store = TodoStore::instance();
    const auto old_todos = store.list_todos(ctx.session_id);

    // 保留匹配项的 id：TodoWrite 条目无 id，按 content 匹配旧条目继承 id，
    // 避免与 TaskV2（按 id 管理）混用时全量替换导致 id 断裂
    for (auto& item : parsed) {
        if (item.id.empty()) {
            auto it = std::find_if(old_todos.begin(), old_todos.end(),
                                   [&](const core::todo::TodoItem& o) {
                                       return o.content == item.content && !o.id.empty();
                                   });
            if (it != old_todos.end()) item.id = it->id;
        }
    }

    const auto new_todos = store.replace_todos(ctx.session_id, parsed);

    nlohmann::json result = {
        {"oldTodos", old_todos},
        {"newTodos", new_todos},
    };
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
