/**
 * @file event_bridge.h
 * @brief EventBridge — EventBus 订阅 → ActionQueue 映射
 * @details 事件回调（可能位于后台/事件泵线程）只入队 action；UI 线程每帧 drain。
 *          不持锁碰 UI 状态。见设计文档 §3。
 *          B4：stop() 真正按 token 退订（存储退订 lambda），不依赖外部 bus.clear() 兜底。
 */

#pragma once

#include <functional>
#include <string>
#include <typeindex>
#include <vector>

#include "core/events/i_event_bus.h"
#include "core/events/event_token.h"

#include "bridge/action_queue.h"

namespace ftxtui {

/// @brief 事件桥
class EventBridge {
   public:
    /// @param bus 事件总线（非拥有）
    /// @param queue 动作队列（非拥有；事件回调只向它入队）
    EventBridge(agent::IEventBus& bus, ActionQueue& queue) : m_bus(bus), m_queue(queue) {}

    /// @brief 订阅全部 UI 相关事件
    void start();

    /// @brief 退订所有已注册事件（按 token 精确退订，不误伤其他订阅者）
    void stop();

    /// @brief 设置通知回调：入队后调用，用于唤醒 UI 线程重绘（screen.PostEvent）
    void set_wake_callback(std::function<void()> cb) { m_wake = std::move(cb); }

    /// @brief 入队一个动作（线程安全）
    void push(Action action);

   private:
    /// @brief 订阅一个事件并登记退订 lambda
    template <typename T>
    void subscribe_typed(std::function<void(const T&)> cb) {
        auto token = m_bus.subscribe<T>(std::move(cb));
        // 退订需带上类型；此处以类型擦除的 lambda 捕获类型信息
        m_unsubscribers.push_back([this, token]() { m_bus.unsubscribe<T>(token); });
    }

    agent::IEventBus& m_bus;
    ActionQueue& m_queue;
    std::function<void()> m_wake;

    /// @brief 已注册事件的退订回调（B4：逐个精确退订）
    std::vector<std::function<void()>> m_unsubscribers;
};

}  // namespace ftxtui
