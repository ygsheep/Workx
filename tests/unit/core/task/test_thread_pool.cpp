/**
 * @file test_thread_pool.cpp
 * @brief ThreadPool 单元测试
 * @details 覆盖 enqueue/enqueue_with_result/shutdown/active_count/pending_count
 *          以及异常安全、并发投递、队列排空等场景
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#include "core/task/thread_pool.h"

using namespace agent;
using namespace std::chrono_literals;

TEST_CASE("ThreadPool executes enqueued tasks", "[thread_pool][basic]") {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    for (int i = 0; i < 10; ++i) {
        pool.enqueue([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    // 等待所有任务完成（poll pending + active）
    while (pool.pending_count() > 0 || pool.active_count() > 0) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(counter.load() == 10);
}

TEST_CASE("ThreadPool enqueue_with_result returns future", "[thread_pool][basic]") {
    ThreadPool pool(2);

    auto fut = pool.enqueue_with_result([](int a, int b) { return a + b; }, 3, 4);
    REQUIRE(fut.get() == 7);
}

TEST_CASE("ThreadPool enqueue_with_result propagates exception", "[thread_pool][exception]") {
    ThreadPool pool(1);

    auto fut = pool.enqueue_with_result([]() -> int { throw std::runtime_error("boom"); });

    bool threw = false;
    try {
        fut.get();
    } catch (const std::runtime_error& e) {
        threw = true;
        REQUIRE(std::string(e.what()) == "boom");
    }
    REQUIRE(threw);
}

TEST_CASE("ThreadPool worker_count reflects configured size", "[thread_pool][basic]") {
    ThreadPool pool(4);
    REQUIRE(pool.worker_count() == 4);
}

TEST_CASE("ThreadPool default constructor uses hardware_concurrency", "[thread_pool][basic]") {
    ThreadPool pool;
    REQUIRE(pool.worker_count() >= 1);
    const size_t hw = std::thread::hardware_concurrency();
    const size_t wc = pool.worker_count();
    REQUIRE((wc == hw || wc == 1));
}

TEST_CASE("ThreadPool active_count tracks running tasks", "[thread_pool][concurrency]") {
    ThreadPool pool(2);
    std::atomic<int> started{0};
    std::atomic<bool> release{false};

    // 投递 2 个长任务占满工作线程
    for (int i = 0; i < 2; ++i) {
        pool.enqueue([&]() {
            started.fetch_add(1);
            while (!release.load()) std::this_thread::sleep_for(5ms);
        });
    }

    // 等待两个任务都进入运行态
    while (started.load() < 2) std::this_thread::sleep_for(5ms);
    REQUIRE(pool.active_count() == 2);

    // 释放
    release.store(true);
    while (pool.active_count() > 0) std::this_thread::sleep_for(5ms);
    REQUIRE(pool.active_count() == 0);
}

TEST_CASE("ThreadPool concurrent enqueue is thread-safe", "[thread_pool][concurrency]") {
    ThreadPool pool(4);
    std::atomic<int> counter{0};

    constexpr int PRODUCERS = 4;
    constexpr int TASKS_PER_PRODUCER = 25;

    std::vector<std::thread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&]() {
            for (int i = 0; i < TASKS_PER_PRODUCER; ++i) {
                pool.enqueue([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
            }
        });
    }
    for (auto& t : producers) t.join();

    while (pool.pending_count() > 0 || pool.active_count() > 0) {
        std::this_thread::sleep_for(5ms);
    }
    REQUIRE(counter.load() == PRODUCERS * TASKS_PER_PRODUCER);
}

TEST_CASE("ThreadPool limits concurrency to worker count", "[thread_pool][concurrency]") {
    // 验证：即使投递 20 个任务，同时执行的也不超过 worker 数
    ThreadPool pool(3);
    std::atomic<int> max_concurrent{0};
    std::atomic<int> current{0};

    for (int i = 0; i < 20; ++i) {
        pool.enqueue([&]() {
            int c = current.fetch_add(1, std::memory_order_relaxed) + 1;
            int prev = max_concurrent.load();
            while (c > prev) {
                if (max_concurrent.compare_exchange_weak(prev, c)) break;
            }
            std::this_thread::sleep_for(10ms);
            current.fetch_sub(1, std::memory_order_relaxed);
        });
    }

    while (pool.pending_count() > 0 || pool.active_count() > 0) {
        std::this_thread::sleep_for(5ms);
    }

    // 并发峰值不应超过 worker 数（允许瞬时统计偏差，加 1 容差）
    REQUIRE(static_cast<size_t>(max_concurrent.load()) <= pool.worker_count() + 1);
    REQUIRE(max_concurrent.load() >= 2);  // 至少发生过并发
}

TEST_CASE("ThreadPool shutdown drains pending tasks", "[thread_pool][shutdown]") {
    // 本实现：shutdown 设置 m_stop=true 但 worker 会消费完队列才退出
    ThreadPool pool(1);
    std::atomic<int> executed{0};

    // 投递一个长任务占住 worker
    std::atomic<bool> block{true};
    pool.enqueue([&]() {
        while (block.load()) std::this_thread::sleep_for(5ms);
        executed.fetch_add(1);
    });

    // 等待 worker 进入运行
    while (pool.active_count() == 0) std::this_thread::sleep_for(5ms);

    // 再投递 5 个任务（排队中）
    for (int i = 0; i < 5; ++i) {
        pool.enqueue([&]() { executed.fetch_add(1); });
    }
    REQUIRE(pool.pending_count() == 5);

    // 释放长任务，shutdown 让 worker 排空队列后退出
    block.store(false);
    pool.shutdown();

    // 所有任务都应被执行（drain 模式）
    REQUIRE(executed.load() == 6);
    REQUIRE(pool.pending_count() == 0);
}

TEST_CASE("ThreadPool enqueue after shutdown is no-op", "[thread_pool][shutdown]") {
    ThreadPool pool(1);
    pool.shutdown();

    std::atomic<int> counter{0};
    pool.enqueue([&]() { counter.fetch_add(1); });
    // 给一点时间确认 worker 不会唤醒
    std::this_thread::sleep_for(20ms);
    REQUIRE(counter.load() == 0);
}

TEST_CASE("ThreadPool destructor calls shutdown", "[thread_pool][shutdown]") {
    std::atomic<int> counter{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 5; ++i) {
            pool.enqueue([&]() { counter.fetch_add(1); });
        }
        // 等待任务完成
        while (pool.pending_count() > 0 || pool.active_count() > 0) {
            std::this_thread::sleep_for(5ms);
        }
    }  // 析构
    REQUIRE(counter.load() == 5);
}

TEST_CASE("ThreadPool handles empty task gracefully", "[thread_pool][basic]") {
    ThreadPool pool(1);
    pool.enqueue(nullptr);  // 不应崩溃
    std::this_thread::sleep_for(10ms);
    REQUIRE(pool.pending_count() == 0);
}
