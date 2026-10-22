/**
 * @file todo_store.cpp
 * @brief TodoStore 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/TodoStore/todo_store.h"

#include <algorithm>
#include <utility>

#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"

namespace agent::tool {

TodoStore& TodoStore::instance() {
    static TodoStore store;
    return store;
}

void TodoStore::set_event_bus(agent::IEventBus* bus) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_event_bus = bus;
}

void TodoStore::notify_changed(const std::string& session_id,
                               const std::vector<core::todo::TodoItem>& todos) {
    // 锁外执行：发布事件 + 持久化回调（避免回调中再次获取锁导致死锁）
    agent::IEventBus* bus = nullptr;
    std::function<void(const std::vector<core::todo::TodoItem>&)> persist_cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        bus = m_event_bus;
        auto it = m_sessions.find(session_id);
        if (it != m_sessions.end()) persist_cb = it->second.persist_cb;
    }
    if (bus) {
        bus->publish_async(agent::TodoUpdatedEvent{
            .session_id = session_id,
            .todos = todos,
        });
    }
    if (persist_cb) persist_cb(todos);
}

std::string TodoStore::create_todo(const std::string& session_id,
                                   const core::todo::TodoItem& item) {
    std::vector<core::todo::TodoItem> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& st = m_sessions[session_id];
        core::todo::TodoItem copy = item;
        copy.id = std::to_string(st.next_id++);
        st.todos.push_back(std::move(copy));
        snapshot = st.todos;
    }
    notify_changed(session_id, snapshot);
    return snapshot.back().id;
}

std::optional<core::todo::TodoItem> TodoStore::get_todo(const std::string& session_id,
                                                        const std::string& id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) return std::nullopt;
    for (const auto& t : it->second.todos) {
        if (t.id == id) return t;
    }
    return std::nullopt;
}

bool TodoStore::update_todo(const std::string& session_id, const std::string& id,
                            const std::function<void(core::todo::TodoItem&)>& mutate) {
    std::vector<core::todo::TodoItem> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(session_id);
        if (it == m_sessions.end()) return false;
        auto& todos = it->second.todos;
        auto found = std::find_if(todos.begin(), todos.end(),
                                  [&](const core::todo::TodoItem& t) { return t.id == id; });
        if (found == todos.end()) return false;
        if (mutate) mutate(*found);
        snapshot = todos;
    }
    notify_changed(session_id, snapshot);
    return true;
}

bool TodoStore::delete_todo(const std::string& session_id, const std::string& id) {
    std::vector<core::todo::TodoItem> snapshot;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(session_id);
        if (it == m_sessions.end()) return false;
        auto& todos = it->second.todos;
        auto new_end =
            std::remove_if(todos.begin(), todos.end(), [&](const core::todo::TodoItem& t) {
                if (t.id == id) {
                    found = true;
                    return true;
                }
                return false;
            });
        if (found) todos.erase(new_end, todos.end());
        snapshot = todos;
    }
    if (found) notify_changed(session_id, snapshot);
    return found;
}

std::vector<core::todo::TodoItem> TodoStore::list_todos(const std::string& session_id) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_sessions.find(session_id);
    if (it == m_sessions.end()) return {};
    return it->second.todos;
}

std::vector<core::todo::TodoItem> TodoStore::replace_todos(
    const std::string& session_id, const std::vector<core::todo::TodoItem>& todos) {
    std::vector<core::todo::TodoItem> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& st = m_sessions[session_id];
        // 全部 completed 时置空（对齐 cc allDone ? [] : todos）
        const bool all_done =
            !todos.empty() &&
            std::all_of(todos.begin(), todos.end(), [](const core::todo::TodoItem& t) {
                return t.status == core::todo::TodoStatus::Completed;
            });
        st.todos = all_done ? std::vector<core::todo::TodoItem>{} : todos;
        snapshot = st.todos;
    }
    notify_changed(session_id, snapshot);
    return snapshot;
}

void TodoStore::set_persist_callback(
    const std::string& session_id,
    std::function<void(const std::vector<core::todo::TodoItem>&)> cb) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessions[session_id].persist_cb = std::move(cb);
}

void TodoStore::restore_todos(const std::string& session_id,
                              std::vector<core::todo::TodoItem> todos) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto& st = m_sessions[session_id];
        st.todos = std::move(todos);
        // 恢复后 next_id 需超过现有最大 id，避免 id 复用
        int max_id = 0;
        for (const auto& t : st.todos) {
            try {
                max_id = std::max(max_id, std::stoi(t.id));
            } catch (...) {
            }
        }
        st.next_id = max_id + 1;
    }
    notify_changed(session_id, list_todos(session_id));
}

void TodoStore::reset_session(const std::string& session_id) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_sessions.find(session_id);
        if (it == m_sessions.end()) return;
        it->second.todos.clear();
        it->second.next_id = 1;
    }
    // 锁外：写空快照（JSONL，防止 /resume 恢复旧清单）+ 发布事件（UI 清空）
    notify_changed(session_id, {});
}

void TodoStore::clear_for_test() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sessions.clear();
    m_event_bus = nullptr;
}

}  // namespace agent::tool
