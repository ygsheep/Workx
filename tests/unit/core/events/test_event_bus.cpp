/**
 * @file test_event_bus.cpp
 * @brief EventBus 单元测试
 * @details 覆盖 subscribe/unsubscribe/publish/publish_async/process_async_events/clear
 *          以及异常安全、多订阅者、跨线程发布等场景。
 *          H-2：EventGuard<T> 模板已删除，相关测试用例同步移除。
 *          L-2：不再覆盖 RAII 守卫（H-2 删除），如需 RAII 请使用 EventToken + 手动 unsubscribe。
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#include "core/events/event_bus.h"

using namespace agent;
using namespace std::chrono_literals;

namespace {

/// @brief 测试用事件类型
struct TestEvent {
    int value = 0;
};

struct AnotherEvent {
    std::string msg;
};

/// @brief 每个测试前清理 EventBus 单例残留订阅
struct EventBusFixture {
    EventBusFixture() { EventBus::instance().clear(); }
    ~EventBusFixture() { EventBus::instance().clear(); }
};

}  // namespace

// ============================================================================
// Basic subscribe & publish
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus single subscriber receives sync event",
                 "[event_bus][basic]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; });

    EventBus::instance().publish(TestEvent{.value = 42});

    REQUIRE(received == 42);
    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus multiple subscribers all receive event",
                 "[event_bus][basic]") {
    std::vector<int> received_a;
    std::vector<int> received_b;

    auto token_a = EventBus::instance().subscribe<TestEvent>(
        [&received_a](const TestEvent& e) { received_a.push_back(e.value); });
    auto token_b = EventBus::instance().subscribe<TestEvent>(
        [&received_b](const TestEvent& e) { received_b.push_back(e.value); });

    EventBus::instance().publish(TestEvent{.value = 1});
    EventBus::instance().publish(TestEvent{.value = 2});

    REQUIRE(received_a.size() == 2);
    REQUIRE(received_a[0] == 1);
    REQUIRE(received_a[1] == 2);
    REQUIRE(received_b.size() == 2);
    REQUIRE(received_b[0] == 1);
    REQUIRE(received_b[1] == 2);

    EventBus::instance().unsubscribe<TestEvent>(token_a);
    EventBus::instance().unsubscribe<TestEvent>(token_b);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus different event types are isolated",
                 "[event_bus][basic]") {
    int test_received = 0;
    int another_received = 0;

    auto t1 = EventBus::instance().subscribe<TestEvent>(
        [&test_received](const TestEvent& e) { test_received = e.value; });
    auto t2 = EventBus::instance().subscribe<AnotherEvent>(
        [&another_received](const AnotherEvent&) { another_received++; });

    EventBus::instance().publish(TestEvent{.value = 100});
    EventBus::instance().publish(AnotherEvent{.msg = "hello"});

    REQUIRE(test_received == 100);
    REQUIRE(another_received == 1);

    EventBus::instance().unsubscribe<TestEvent>(t1);
    EventBus::instance().unsubscribe<AnotherEvent>(t2);
}

// ============================================================================
// Unsubscribe
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus unsubscribe stops receiving events",
                 "[event_bus][unsubscribe]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received += e.value; });

    EventBus::instance().publish(TestEvent{.value = 10});
    REQUIRE(received == 10);

    EventBus::instance().unsubscribe<TestEvent>(token);

    EventBus::instance().publish(TestEvent{.value = 100});
    REQUIRE(received == 10);  // no longer increments
}

TEST_CASE_METHOD(EventBusFixture, "EventBus unsubscribe invalid token does not affect others",
                 "[event_bus][unsubscribe]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; });

    EventToken invalid_token;                                    // default-constructed as invalid
    EventBus::instance().unsubscribe<TestEvent>(invalid_token);  // should not crash

    EventBus::instance().publish(TestEvent{.value = 5});
    REQUIRE(received == 5);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus partial unsubscribe keeps others receiving",
                 "[event_bus][unsubscribe]") {
    int a = 0, b = 0, c = 0;
    auto ta = EventBus::instance().subscribe<TestEvent>([&a](const TestEvent& e) { a = e.value; });
    auto tb = EventBus::instance().subscribe<TestEvent>([&b](const TestEvent& e) { b = e.value; });
    auto tc = EventBus::instance().subscribe<TestEvent>([&c](const TestEvent& e) { c = e.value; });

    EventBus::instance().unsubscribe<TestEvent>(tb);

    EventBus::instance().publish(TestEvent{.value = 7});

    REQUIRE(a == 7);
    REQUIRE(b == 0);  // unsubscribed
    REQUIRE(c == 7);

    EventBus::instance().unsubscribe<TestEvent>(ta);
    EventBus::instance().unsubscribe<TestEvent>(tc);
}

// ============================================================================
// Async publish
// ============================================================================

TEST_CASE_METHOD(EventBusFixture,
                 "EventBus publish_async triggers callback via process_async_events",
                 "[event_bus][async]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; });

    EventBus::instance().publish_async(TestEvent{.value = 99});
    REQUIRE(received == 0);  // not consumed yet

    EventBus::instance().process_async_events();
    REQUIRE(received == 99);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus multiple async events consumed in order",
                 "[event_bus][async]") {
    std::vector<int> received_order;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received_order](const TestEvent& e) { received_order.push_back(e.value); });

    for (int i = 1; i <= 5; ++i) {
        EventBus::instance().publish_async(TestEvent{.value = i});
    }

    REQUIRE(received_order.empty());

    EventBus::instance().process_async_events();

    REQUIRE(received_order.size() == 5);
    REQUIRE(received_order[0] == 1);
    REQUIRE(received_order[4] == 5);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus process_async_events clears queue",
                 "[event_bus][async]") {
    int count = 0;
    auto token = EventBus::instance().subscribe<TestEvent>([&count](const TestEvent&) { count++; });

    EventBus::instance().publish_async(TestEvent{});
    EventBus::instance().publish_async(TestEvent{});
    EventBus::instance().process_async_events();
    REQUIRE(count == 2);

    // second process should have no new events
    EventBus::instance().process_async_events();
    REQUIRE(count == 2);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

// ============================================================================
// clear (H-2：EventGuard 测试已删除)
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus clear removes all subscribers and async queue",
                 "[event_bus][clear]") {
    int received = 0;
    [[maybe_unused]] auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; });

    EventBus::instance().publish_async(TestEvent{.value = 50});
    EventBus::instance().clear();

    // after clear, sync publish should not trigger (subscribers cleared)
    EventBus::instance().publish(TestEvent{.value = 999});
    REQUIRE(received == 0);

    // async queue should also be empty
    EventBus::instance().process_async_events();
    REQUIRE(received == 0);
}

// ============================================================================
// Exception safety
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus single callback throwing does not break others",
                 "[event_bus][exception]") {
    int a = 0, c = 0;
    auto ta = EventBus::instance().subscribe<TestEvent>([&a](const TestEvent& e) {
        a = e.value;
        throw std::runtime_error("callback A failed");
    });
    auto tb = EventBus::instance().subscribe<TestEvent>(
        [](const TestEvent&) { throw std::runtime_error("callback B failed"); });
    auto tc = EventBus::instance().subscribe<TestEvent>([&c](const TestEvent& e) { c = e.value; });

    EventBus::instance().publish(TestEvent{.value = 7});

    REQUIRE(a == 7);
    REQUIRE(c == 7);  // C still called even though B threw

    EventBus::instance().unsubscribe<TestEvent>(ta);
    EventBus::instance().unsubscribe<TestEvent>(tb);
    EventBus::instance().unsubscribe<TestEvent>(tc);
}

TEST_CASE_METHOD(
    EventBusFixture,
    "EventBus sync publish does not hold lock during callback (no reentrancy deadlock)",
    "[event_bus][reentrancy]") {
    // Verifies T-1 fix: publish copies callback list and releases lock before invoking
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>([&received](const TestEvent& e) {
        received = e.value;
        // publishing same-type event inside callback should not deadlock
        if (e.value < 3) {
            EventBus::instance().publish(TestEvent{.value = e.value + 1});
        }
    });

    EventBus::instance().publish(TestEvent{.value = 1});
    // recursive publish should have driven received to 3
    REQUIRE(received == 3);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

// ============================================================================
// Cross-thread publishing
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus concurrent publish_async is thread-safe",
                 "[event_bus][thread]") {
    std::atomic<int> received{0};
    auto token =
        EventBus::instance().subscribe<TestEvent>([&received](const TestEvent&) { received++; });

    constexpr int THREAD_COUNT = 4;
    constexpr int EVENTS_PER_THREAD = 25;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([]() {
            for (int i = 0; i < EVENTS_PER_THREAD; ++i) {
                EventBus::instance().publish_async(TestEvent{});
            }
        });
    }
    for (auto& th : threads) th.join();

    EventBus::instance().process_async_events();

    REQUIRE(received.load() == THREAD_COUNT * EVENTS_PER_THREAD);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus cross-thread subscribe and publish",
                 "[event_bus][thread]") {
    std::atomic<int> received{0};
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received.store(e.value); });

    std::thread publisher([]() { EventBus::instance().publish(TestEvent{.value = 777}); });

    publisher.join();
    REQUIRE(received.load() == 777);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

// ============================================================================
// EventToken behavior
// ============================================================================

TEST_CASE("EventToken default-constructed is invalid", "[event_bus][token]") {
    EventToken token;
    REQUIRE_FALSE(token.is_valid());
    REQUIRE(token.get_id() == 0);
}

TEST_CASE("EventToken invalidate marks as invalid", "[event_bus][token]") {
    EventToken token(123);
    REQUIRE(token.is_valid());
    REQUIRE(token.get_id() == 123);

    token.invalidate();
    REQUIRE_FALSE(token.is_valid());
}

TEST_CASE("EventToken move transfers ownership", "[event_bus][token]") {
    EventToken token_a(42);
    EventToken token_b(std::move(token_a));

    REQUIRE(token_b.is_valid());
    REQUIRE(token_b.get_id() == 42);
    REQUIRE_FALSE(token_a.is_valid());  // moved-from
}

// ============================================================
// M-8: drain_async_events 排空 API
// ============================================================

TEST_CASE("drain_async_events processes all queued events (M-8)", "[event_bus][m8][drain]") {
    auto& bus = EventBus::instance();
    bus.clear();

    std::atomic<int> received{0};
    auto token = bus.subscribe<TestEvent>(
        [&received](const TestEvent&) { received.fetch_add(1, std::memory_order_relaxed); });

    // 入队 5 个异步事件
    for (int i = 0; i < 5; ++i) {
        bus.publish_async(TestEvent{.value = i});
    }
    REQUIRE(bus.async_queue_size() == 5);
    REQUIRE(received.load() == 0);  // 未 drain，不应派发

    // drain：应处理完所有事件
    size_t iterations = bus.drain_async_events();
    REQUIRE(iterations == 1);  // 单批次即可排空（5 个事件一次性处理）
    REQUIRE(bus.async_queue_size() == 0);
    REQUIRE(received.load() == 5);

    bus.unsubscribe<TestEvent>(token);
    bus.clear();
}

TEST_CASE("drain_async_events handles chained publish_async (M-8)", "[event_bus][m8][drain]") {
    // 场景：回调中再次 publish_async，drain 应多批次排空直到无积压
    auto& bus = EventBus::instance();
    bus.clear();

    std::atomic<int> received{0};
    std::atomic<int> depth{0};
    constexpr int TARGET_DEPTH = 3;

    auto token = bus.subscribe<TestEvent>([&](const TestEvent&) {
        received.fetch_add(1, std::memory_order_relaxed);
        int d = depth.fetch_add(1, std::memory_order_relaxed) + 1;
        if (d < TARGET_DEPTH) {
            bus.publish_async(TestEvent{.value = d});  // 链式入队
        }
    });

    bus.publish_async(TestEvent{.value = 0});
    REQUIRE(bus.async_queue_size() == 1);

    // drain：链式 publish_async 需要多次批次排空
    size_t iterations = bus.drain_async_events();
    REQUIRE(iterations >= TARGET_DEPTH);  // 至少 TARGET_DEPTH 批次
    REQUIRE(bus.async_queue_size() == 0);
    REQUIRE(received.load() == TARGET_DEPTH);
    REQUIRE(depth.load() == TARGET_DEPTH);

    bus.unsubscribe<TestEvent>(token);
    bus.clear();
}

TEST_CASE("drain_async_events respects max_iterations bound (M-8)", "[event_bus][m8][drain]") {
    // 场景：回调无限 publish_async，drain 应在 max_iterations 兜底返回
    auto& bus = EventBus::instance();
    bus.clear();

    std::atomic<int> received{0};
    auto token = bus.subscribe<TestEvent>([&](const TestEvent&) {
        received.fetch_add(1, std::memory_order_relaxed);
        bus.publish_async(TestEvent{});  // 永远再入队一个
    });

    bus.publish_async(TestEvent{});
    size_t iterations = bus.drain_async_events(4);
    REQUIRE(iterations == 4);
    REQUIRE(bus.async_queue_size() > 0);  // 仍有积压（兜底退出）
    REQUIRE(received.load() == 4);

    bus.unsubscribe<TestEvent>(token);
    bus.clear();
}

TEST_CASE("drain_async_events on empty queue is no-op (M-8)", "[event_bus][m8][drain]") {
    auto& bus = EventBus::instance();
    bus.clear();
    REQUIRE(bus.async_queue_size() == 0);
    REQUIRE(bus.drain_async_events() == 0);  // 空队列，0 次迭代
    bus.clear();
}

// ============================================================================
// 诊断接口（G-1）：测试失败时可用于输出 EventBus 内部状态
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus async_queue_size reflects queue backlog",
                 "[event_bus][debug]") {
    // 初始队列为空
    REQUIRE(EventBus::instance().async_queue_size() == 0);

    // 入队 3 个异步事件，未消费
    EventBus::instance().publish_async(TestEvent{.value = 1});
    EventBus::instance().publish_async(TestEvent{.value = 2});
    EventBus::instance().publish_async(TestEvent{.value = 3});
    REQUIRE(EventBus::instance().async_queue_size() == 3);

    // 消费后队列清空
    EventBus::instance().process_async_events();
    REQUIRE(EventBus::instance().async_queue_size() == 0);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus subscriber_count tracks subscribe/unsubscribe",
                 "[event_bus][debug]") {
    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 0);
    REQUIRE(EventBus::instance().subscriber_count<AnotherEvent>() == 0);
    REQUIRE(EventBus::instance().total_subscriber_count() == 0);

    auto t1 = EventBus::instance().subscribe<TestEvent>([](const TestEvent&) {});
    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 1);
    REQUIRE(EventBus::instance().total_subscriber_count() == 1);

    auto t2 = EventBus::instance().subscribe<TestEvent>([](const TestEvent&) {});
    auto t3 = EventBus::instance().subscribe<AnotherEvent>([](const AnotherEvent&) {});
    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 2);
    REQUIRE(EventBus::instance().subscriber_count<AnotherEvent>() == 1);
    REQUIRE(EventBus::instance().total_subscriber_count() == 3);

    EventBus::instance().unsubscribe<TestEvent>(t1);
    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 1);
    REQUIRE(EventBus::instance().total_subscriber_count() == 2);

    EventBus::instance().unsubscribe<TestEvent>(t2);
    EventBus::instance().unsubscribe<AnotherEvent>(t3);
    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 0);
    REQUIRE(EventBus::instance().subscriber_count<AnotherEvent>() == 0);
    REQUIRE(EventBus::instance().total_subscriber_count() == 0);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus clear resets all debug counters",
                 "[event_bus][debug]") {
    [[maybe_unused]] auto t = EventBus::instance().subscribe<TestEvent>([](const TestEvent&) {});
    EventBus::instance().publish_async(TestEvent{});
    EventBus::instance().publish_async(TestEvent{});

    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 1);
    REQUIRE(EventBus::instance().async_queue_size() == 2);

    EventBus::instance().clear();

    REQUIRE(EventBus::instance().subscriber_count<TestEvent>() == 0);
    REQUIRE(EventBus::instance().async_queue_size() == 0);
    REQUIRE(EventBus::instance().total_subscriber_count() == 0);
}
