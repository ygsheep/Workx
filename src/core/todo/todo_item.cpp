/**
 * @file todo_item.cpp
 * @brief TodoItem 实现（状态转换 + JSON 序列化）
 * @version 1.0.0
 * @date 2026-08
 */

#include "core/todo/todo_item.h"

namespace core::todo {

const char* TodoItem::status_str(TodoStatus s) {
    switch (s) {
        case TodoStatus::Pending:
            return "pending";
        case TodoStatus::InProgress:
            return "in_progress";
        case TodoStatus::Completed:
            return "completed";
    }
    return "pending";
}

TodoStatus TodoItem::status_from(const std::string& s) {
    if (s == "in_progress") return TodoStatus::InProgress;
    if (s == "completed") return TodoStatus::Completed;
    return TodoStatus::Pending;
}

void to_json(nlohmann::json& j, const TodoItem& item) {
    j = nlohmann::json{
        {"id", item.id},
        {"content", item.content},
        {"activeForm", item.active_form},
        {"status", TodoItem::status_str(item.status)},
        {"description", item.description},
        {"owner", item.owner},
        {"blocks", item.blocks},
        {"blockedBy", item.blocked_by},
        {"metadata", item.metadata.is_null() ? nlohmann::json::object() : item.metadata},
    };
}

void from_json(const nlohmann::json& j, TodoItem& item) {
    item.id = j.value("id", std::string{});
    item.content = j.value("content", std::string{});
    item.active_form = j.value("activeForm", std::string{});
    item.status = TodoItem::status_from(j.value("status", std::string{"pending"}));
    item.description = j.value("description", std::string{});
    item.owner = j.value("owner", std::string{});
    item.blocks = j.value("blocks", std::vector<std::string>{});
    item.blocked_by = j.value("blockedBy", std::vector<std::string>{});
    if (j.contains("metadata") && j["metadata"].is_object()) {
        item.metadata = j["metadata"];
    } else {
        item.metadata = nlohmann::json::object();
    }
}

}  // namespace core::todo
