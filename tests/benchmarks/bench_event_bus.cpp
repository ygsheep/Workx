/**
 * @file bench_event_bus.cpp
 * @brief EventBus 性能基准（Q-2）
 * @details 测量：
 *          - 单线程 publish 吞吐（sync vs async）
 *          - 多订阅者开销
 *          - 订阅/退订开销
 * @note EventBus 为 Meyers Singleton（构造函数私有），benchmark 通过
 *       EventBus::instance() 复用单例，并在每个用例结束时 clear() 重置状态，
 *       避免跨用例订阅者累积。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include "core/events/event_bus.h"
#include "core/events/event_token.h"

#include <vector>

using namespace agent;

namespace {

struct BenchEvent {
    int payload;
};

}  // anonymous namespace

TEST_CASE("EventBus single-thread publish throughput", "[benchmark][event_bus]") {
    EventBus& bus = EventBus::instance();
    bus.clear();

    int counter = 0;
    auto token = bus.subscribe<BenchEvent>([&](const BenchEvent& e) { counter += e.payload; });

    const int N = 100'000;
    BENCHMARK("publish_async 100k events") {
        for (int i = 0; i < N; ++i) {
            bus.publish_async(BenchEvent{.payload = 1});
        }
        bus.process_async_events();
        return counter;
    };

    BENCHMARK("publish_sync 100k events") {
        for (int i = 0; i < N; ++i) {
            bus.publish(BenchEvent{.payload = 1});
        }
        return counter;
    };

    bus.unsubscribe<BenchEvent>(token);
    bus.clear();
}

TEST_CASE("EventBus multi-subscriber overhead", "[benchmark][event_bus]") {
    EventBus& bus = EventBus::instance();
    bus.clear();

    constexpr int SUBSCRIBER_COUNT = 10;
    std::vector<int> counters(SUBSCRIBER_COUNT, 0);
    std::vector<EventToken> tokens;

    for (int i = 0; i < SUBSCRIBER_COUNT; ++i) {
        tokens.push_back(bus.subscribe<BenchEvent>(
            [i, &counters](const BenchEvent& e) { counters[i] += e.payload; }));
    }

    BENCHMARK("publish_sync 10k events x 10 subscribers") {
        for (int i = 0; i < 10'000; ++i) {
            bus.publish(BenchEvent{.payload = 1});
        }
        return counters[0];
    };

    for (auto& t : tokens) bus.unsubscribe<BenchEvent>(t);
    bus.clear();
}

TEST_CASE("EventBus subscribe/unsubscribe overhead", "[benchmark][event_bus]") {
    EventBus& bus = EventBus::instance();
    bus.clear();

    BENCHMARK("subscribe + unsubscribe 1000 times") {
        std::vector<EventToken> tokens;
        for (int i = 0; i < 1000; ++i) {
            tokens.push_back(bus.subscribe<BenchEvent>([](const BenchEvent&) {}));
        }
        for (auto& t : tokens) bus.unsubscribe<BenchEvent>(t);
        return tokens.size();
    };

    bus.clear();
}
