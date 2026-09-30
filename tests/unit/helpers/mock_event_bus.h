/**
 * @file mock_event_bus.h
 * @brief 测试用 Mock IEventBus
 * @details 记录所有订阅/发布/取消订阅操作，不实际派发事件回调（除非显式启用）。
 *          供需要注入 IEventBus 的组件（Terminal/Client/TaskManager 等）做隔离测试。
 *
 * 使用示例：
 * @code
 *   using namespace agent::test;
 *   MockEventBus bus;
 *   bus.publish(UserInputEvent{.text = "hello"});
 *   REQUIRE(bus.published_count<UserInputEvent>() == 1);
 * @endcode
 */

#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <typeindex>
#include <vector>
#include <functional>
#include <string>

#include "core/events/i_event_bus.h"

namespace agent::test {

/// @brief Mock IEventBus
/// @details 线程安全（内部互斥锁）。默认不派发回调，仅记录操作；
///          调用 set_dispatch_enabled(true) 后 publish_raw 会同步派发。
class MockEventBus final : public IEventBus {
   public:
    MockEventBus() = default;
    ~MockEventBus() override = default;

    MockEventBus(const MockEventBus&) = delete;
    MockEventBus& operator=(const MockEventBus&) = delete;

    // === IEventBus 实现 ===

    EventToken subscribe_raw(std::type_index type,
                             std::function<void(const void*)> callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        EventToken token = make_token();
        m_subscribers[type].push_back({token, std::move(callback)});
        return token;
    }

    void unsubscribe_raw(std::type_index type, const EventToken& token) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(type);
        if (it == m_subscribers.end()) return;
        auto& vec = it->second;
        vec.erase(
            std::remove_if(vec.begin(), vec.end(),
                           [&](const Subscriber& s) { return s.token.get_id() == token.get_id(); }),
            vec.end());
        ++m_unsubscribe_count;
    }

    void publish_raw(std::type_index type, const void* event) override {
        std::vector<std::function<void(const void*)>> callbacks;
        bool dispatch;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_published_types.push_back(type);
            if (m_dispatch_enabled) {
                auto it = m_subscribers.find(type);
                if (it != m_subscribers.end()) {
                    for (const auto& s : it->second) {
                        callbacks.push_back(s.callback);
                    }
                }
            }
            dispatch = m_dispatch_enabled;
        }
        // 派发在锁外执行，避免回调中再次调用导致死锁
        if (dispatch) {
            for (const auto& cb : callbacks) {
                cb(event);
            }
        }
    }

    void publish_async_raw(std::type_index type, std::function<void()> emitter) override {
        // auto-flush 模式：模拟宿主即时响应（工具阻塞等待 result_promise 时用）
        if (m_async_auto_flush) {
            emitter();  // 锁外调用：emitter 内部 publish_raw 会自行加锁
            return;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_async_queue.push_back({type, std::move(emitter)});
    }

    void process_async_events() override {
        std::vector<AsyncItem> items;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            items.swap(m_async_queue);
        }
        for (auto& item : items) {
            if (item.emitter) item.emitter();
        }
    }

    void clear() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_subscribers.clear();
        m_async_queue.clear();
        m_published_types.clear();
        m_unsubscribe_count = 0;
    }

    [[nodiscard]] size_t async_queue_size() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_async_queue.size();
    }

    [[nodiscard]] size_t total_subscriber_count() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t total = 0;
        for (const auto& [_, vec] : m_subscribers) {
            total += vec.size();
        }
        return total;
    }

    // === 诊断 / 断言辅助 API ===

    /// @brief 启用/禁用回调派发（默认禁用，仅记录操作）
    void set_dispatch_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dispatch_enabled = enabled;
    }

    /// @brief 启用/禁用异步事件自动派发（默认禁用，入队等待 process_async_events）
    /// @details 启用后 publish_async 立即执行 emitter（同步派发），
    ///          用于模拟宿主即时响应工具线程的阻塞等待场景（如 AskUserRequestEvent）。
    void set_async_auto_flush(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_async_auto_flush = enabled;
    }

    /// @brief 获取某类型事件的发布次数
    template <typename T>
    [[nodiscard]] size_t published_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto target = std::type_index(typeid(T));
        return std::count(m_published_types.begin(), m_published_types.end(), target);
    }

    /// @brief 获取所有类型事件的总发布次数
    [[nodiscard]] size_t total_published_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_published_types.size();
    }

    /// @brief 获取某类型事件的订阅者数量
    template <typename T>
    [[nodiscard]] size_t subscriber_count_typed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(std::type_index(typeid(T)));
        return it == m_subscribers.end() ? 0 : it->second.size();
    }

    /// @brief 获取 unsubscribe 总调用次数
    [[nodiscard]] size_t unsubscribe_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_unsubscribe_count;
    }

   protected:
    [[nodiscard]] size_t subscriber_count_typed(std::type_index type) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_subscribers.find(type);
        return it == m_subscribers.end() ? 0 : it->second.size();
    }

   private:
    struct Subscriber {
        EventToken token;
        std::function<void(const void*)> callback;
    };

    struct AsyncItem {
        std::type_index type;
        std::function<void()> emitter;
    };

    mutable std::mutex m_mutex;
    std::map<std::type_index, std::vector<Subscriber>> m_subscribers;
    std::vector<AsyncItem> m_async_queue;
    std::vector<std::type_index> m_published_types;
    size_t m_unsubscribe_count = 0;
    bool m_dispatch_enabled = false;
    bool m_async_auto_flush = false;

    static EventToken make_token() {
        static std::atomic<EventToken::ID> counter{1};
        return EventToken(counter.fetch_add(1, std::memory_order_relaxed));
    }
};

}  // namespace agent::test
