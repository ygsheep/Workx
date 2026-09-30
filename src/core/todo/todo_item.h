/**
 * @file todo_item.h
 * @brief 待办条目规范类型（core 层，agent/ftxtui/events 共用）
 * @details 对齐 core/tool_kind.h 的 C-3 模式：core 放纯数据结构，
 *          agent 层引用，避免 core→agent 分层越界。
 *          含 JSON 序列化（SessionStore 持久化 / 工具 schema 共用）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace core::todo {

/// @brief 待办状态（对齐 cc TaskStatusSchema）
enum class TodoStatus : uint8_t {
    Pending = 0,     ///< pending：未开始
    InProgress = 1,  ///< in_progress：进行中
    Completed = 2,   ///< completed：已完成
};

/// @brief 待办条目
struct TodoItem {
    std::string id;           ///< TaskV2: "1","2",...；TodoWrite: 空
    std::string content;      ///< 命令式措辞（subject），如 "Run tests"
    std::string active_form;  ///< 进行式措辞，如 "Running tests"（in_progress 时显示）
    TodoStatus status{TodoStatus::Pending};
    std::string description;              ///< TaskV2 可选
    std::string owner;                    ///< TaskV2 可选（本期不用于协作）
    std::vector<std::string> blocks;      ///< 本任务阻塞的任务 id
    std::vector<std::string> blocked_by;  ///< 阻塞本任务的任务 id
    nlohmann::json metadata;              ///< 任意元数据

    /// @brief 状态 → 字符串（"pending"/"in_progress"/"completed"）
    static const char* status_str(TodoStatus s);

    /// @brief 字符串 → 状态（非法返回 Pending）
    static TodoStatus status_from(const std::string& s);

    /// @brief 值相等（供 ViewModel 去重/比较 Todo 列表变化）
    bool operator==(const TodoItem& o) const {
        return id == o.id && content == o.content && active_form == o.active_form &&
               status == o.status && description == o.description && owner == o.owner &&
               blocks == o.blocks && blocked_by == o.blocked_by;
    }
};

/// @brief 序列化到 JSON（SessionStore / 工具返回共用）
void to_json(nlohmann::json& j, const TodoItem& item);

/// @brief 从 JSON 反序列化（缺省字段用默认值）
void from_json(const nlohmann::json& j, TodoItem& item);

}  // namespace core::todo
