/**
 * @file i_event_bus.h
 * @brief 事件总线抽象接口（D-1 DI 化）
 * @details 类型擦除的虚函数接口 + 模板包装，允许测试注入 MockEventBus，
 *          解除对 EventBus 单例的硬依赖。
 *          类型擦除虚函数使用 _raw 后缀，避免与模板包装同名导致 MSVC 解析歧义。
 *
 *          M-8：异步事件驱动约束文档化 ——
 *          publish_async_* 仅入队，不立即派发；必须由主循环（TUI 主线程、
 *          Agent step 循环）显式调用 process_async_events() / drain_async_events()
 *          才会同步派发到订阅者。这是单线程派发模型的核心契约：
 *            - 异步事件永远在主循环线程被处理，订阅者无需加锁
 *            - 跨线程发布通过入队 + 主循环 drain 解耦
 *          违反此契约（如未在主循环调用 drain）会导致异步事件积压，
 *          表现为 UI 不更新 / 状态延迟。诊断接口 async_queue_size() 可观测积压。
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <functional>
#include <typeindex>
#include <cstddef>

#include "core/export.h"

#include "core/events/event_token.h"

namespace agent {

/// @brief 事件总线抽象接口
/// @details 提供类型擦除的虚函数接口供 DI 注入；模板包装方法委托虚函数，
///          调用方可像使用 EventBus 一样使用 IEventBus&。
class WORKX_API IEventBus {
   public:
    virtual ~IEventBus() = default;

    // === 类型擦除的虚函数（由实现类提供，使用 _raw 后缀避免与模板包装冲突）===

    /// @brief 订阅事件（类型擦除）
    /// @param type 事件类型（typeid(T)）
    /// @param callback 回调函数，接收 const void* 指向事件对象
    /// @return 订阅令牌
    virtual EventToken subscribe_raw(std::type_index type,
                                     std::function<void(const void*)> callback) = 0;

    /// @brief 取消订阅
    virtual void unsubscribe_raw(std::type_index type, const EventToken& token) = 0;

    /// @brief 同步发布事件
    virtual void publish_raw(std::type_index type, const void* event) = 0;

    /// @brief 异步发布事件（入队，等 process_async_events 处理）
    /// @param emitter 实际发布事件的函数对象（捕获事件拷贝并调用 publish_raw）
    /// @par M-8 异步驱动约束
    /// 本方法仅入队，不立即派发。必须由主循环调用 process_async_events() /
    /// drain_async_events() 才会派发到订阅者。详见文件头说明。
    virtual void publish_async_raw(std::type_index type, std::function<void()> emitter) = 0;

    /// @brief 处理当前队列中的异步事件（单批次）
    /// @details 取出当前队列快照并逐个派发。新派发过程中再次 publish_async
    /// 的事件会进入新批次，本调用不会处理。需要排空到无积压时使用 drain_async_events()。
    /// @par M-8 异步驱动约束
    /// 必须由主循环线程调用，保证订阅者回调在主线程执行。
    virtual void process_async_events() = 0;

    /// @brief 清空所有订阅与异步队列
    virtual void clear() = 0;

    // === 诊断接口 ===

    [[nodiscard]] virtual size_t async_queue_size() const = 0;
    [[nodiscard]] virtual size_t total_subscriber_count() const = 0;

    // === 非虚工具方法（M-8：测试 API）===

    /// @brief 排空异步事件队列（M-8：测试 API）
    /// @details 循环调用 process_async_events() 直到 async_queue_size() 为 0，
    ///          或达到 max_iterations 兜底（防止回调再次 publish_async 形成无限循环）。
    ///          测试用例在断言前调用本方法，确保所有异步事件已派发到订阅者。
    /// @param max_iterations 最大迭代次数（默认 16），达到后即使仍有积压也返回
    /// @return 实际执行的 process_async_events 次数
    size_t drain_async_events(size_t max_iterations = 16) {
        size_t iterations = 0;
        while (async_queue_size() > 0 && iterations < max_iterations) {
            process_async_events();
            ++iterations;
        }
        return iterations;
    }

    // === 模板包装（非虚，委托 _raw 虚函数）===

    /// @brief 订阅事件（类型安全）
    template <typename T>
    EventToken subscribe(std::function<void(const T&)> callback) {
        return subscribe_raw(std::type_index(typeid(T)), [cb = std::move(callback)](const void* p) {
            cb(*static_cast<const T*>(p));
        });
    }

    /// @brief 取消订阅（类型安全）
    template <typename T>
    void unsubscribe(const EventToken& token) {
        unsubscribe_raw(std::type_index(typeid(T)), token);
    }

    /// @brief 同步发布事件（类型安全）
    template <typename T>
    void publish(const T& event) {
        publish_raw(std::type_index(typeid(T)), &event);
    }

    /// @brief 异步发布事件（类型安全）
    /// @details 捕获事件拷贝到 emitter，等 process_async_events 时再同步发布。
    ///          M-8：异步驱动约束详见 publish_async_raw 文档与文件头。
    template <typename T>
    void publish_async(const T& event) {
        publish_async_raw(std::type_index(typeid(T)), [this, event]() {
            this->publish_raw(std::type_index(typeid(T)), &event);
        });
    }

    /// @brief 指定事件类型的订阅者数量
    template <typename T>
    [[nodiscard]] size_t subscriber_count() const {
        return subscriber_count_typed(std::type_index(typeid(T)));
    }

   protected:
    /// @brief 类型擦除的 subscriber_count（供模板包装调用）
    [[nodiscard]] virtual size_t subscriber_count_typed(std::type_index type) const = 0;
};

}  // namespace agent
