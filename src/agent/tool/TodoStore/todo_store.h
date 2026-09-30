/**
 * @file todo_store.h
 * @brief TodoStore — 待办清单单例（按 session 分桶，mutex 保护）
 * @details 对齐 FileHistory 单例范式：工具 call() 为 const 方法、跨线程并行，
 *          内部用 std::mutex 保护。每次变更后：
 *          1. 发布 TodoUpdatedEvent（经 IEventBus，锁外）
 *          2. 调 session 级持久化回调（ChatSession 注入，锁外）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/todo/todo_item.h"

namespace agent {
class IEventBus;
}

namespace agent::tool {

/// @brief TodoStore — 待办清单单例（按 session 分桶）
class TodoStore {
   public:
    static TodoStore& instance();

    // ---- CRUD（V2 细粒度）----

    /// @brief 创建待办，返回自增 id（"1","2",...）
    std::string create_todo(const std::string& session_id, const core::todo::TodoItem& item);

    /// @brief 按 id 读取待办
    std::optional<core::todo::TodoItem> get_todo(const std::string& session_id,
                                                 const std::string& id) const;

    /// @brief 按 id 修改待办（mutate 内修改字段）
    /// @return 是否找到并修改
    bool update_todo(const std::string& session_id, const std::string& id,
                     const std::function<void(core::todo::TodoItem&)>& mutate);

    /// @brief 按 id 删除待办
    bool delete_todo(const std::string& session_id, const std::string& id);

    /// @brief 列出某 session 全部待办（拷贝）
    std::vector<core::todo::TodoItem> list_todos(const std::string& session_id) const;

    // ---- 全量替换（V1 TodoWrite）----

    /// @brief 全量替换某 session 待办列表
    /// @details 传入列表全部 completed 时置空（对齐 cc allDone ? [] : todos）
    /// @return 替换后的列表
    std::vector<core::todo::TodoItem> replace_todos(const std::string& session_id,
                                                    const std::vector<core::todo::TodoItem>& todos);

    // ---- 事件总线 / 持久化接线 ----

    /// @brief 注入事件总线（非拥有；nullptr 时跳过事件发布）
    void set_event_bus(agent::IEventBus* bus);

    /// @brief 注册 session 级持久化回调（ChatSession 写入 SessionStore）
    void set_persist_callback(const std::string& session_id,
                              std::function<void(const std::vector<core::todo::TodoItem>&)> cb);

    /// @brief 恢复内存态（/resume 时 ChatSession 调用，随后发布事件刷新 UI）
    void restore_todos(const std::string& session_id, std::vector<core::todo::TodoItem> todos);

    /// @brief 清空某 session 待办（保留持久化回调），写空快照到 JSONL + 发布事件
    /// @details clear_history 场景：清空后 /resume 不再恢复旧清单，新变更继续持久化
    void reset_session(const std::string& session_id);

    /// @brief 清空全部（测试用）
    void clear_for_test();

   private:
    TodoStore() = default;
    TodoStore(const TodoStore&) = delete;
    TodoStore& operator=(const TodoStore&) = delete;

    struct SessionState {
        std::vector<core::todo::TodoItem> todos;
        int next_id = 1;  ///< 自增 id（对齐 cc high water mark）
        std::function<void(const std::vector<core::todo::TodoItem>&)> persist_cb;
    };

    /// @brief 变更后统一处理：发布事件 + 调持久化回调（锁外调用）
    void notify_changed(const std::string& session_id,
                        const std::vector<core::todo::TodoItem>& todos);

    mutable std::mutex m_mutex;
    agent::IEventBus* m_event_bus = nullptr;
    std::unordered_map<std::string, SessionState> m_sessions;
};

}  // namespace agent::tool
