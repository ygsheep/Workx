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

namespace {

/// @brief 轮询等待 pred 为真，超时即判定测试失败
/// @details 不要直接等待 pending_count()/active_count() 归零：任务从队列出队
///          （pending 递减）与标记运行态（active 递增）之间存在窗口，两个计数器
///          可能同时读到 0 而任务尚未执行完 —— 这正是「concurrent enqueue is
///          thread-safe」在 CI 上偶发失败的根因（#163）。改为轮询测试真正关心的
///          终态（如任务计数到位），并设超时上限：超时即失败，不用死等掩盖回归。
template <typename Pred>
void wait_for(Pred pred, std::chrono::milliseconds timeout, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            FAIL("等待超时：" << what);
        }
        std::this_thread::sleep_for(5ms);
    }
}

}  // namespace

TEST_CASE("ThreadPool executes enqueued tasks", "[thread_pool][basic]") {
    ThreadPool pool(2);
    std::atomic<int> counter{0};

    for (int i = 0; i < 10; ++i) {
        pool.enqueue([&counter]() { counter.fetch_add(1, std::memory_order_relaxed); });
    }

    // 轮询终态（而不是 pending/active 的瞬时读数）
    wait_for([&] { return counter.load() == 10; }, 5s, "10 个任务全部执行完");
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
    wait_for([&] { return started.load() >= 2; }, 5s, "两个长任务都已启动");
    REQUIRE(pool.active_count() == 2);

    // 释放
    release.store(true);
    wait_for([&] { return pool.active_count() == 0; }, 5s, "两个长任务都已结束");
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

    // 轮询终态：全部任务执行完（pending/active 归零存在出队-运行态窗口，见 wait_for 注释）
    wait_for([&] { return counter.load() == PRODUCERS * TASKS_PER_PRODUCER; }, 10s,
             "全部 100 个任务执行完");
    REQUIRE(counter.load() == PRODUCERS * TASKS_PER_PRODUCER);
}

TEST_CASE("ThreadPool limits concurrency to worker count", "[thread_pool][concurrency]") {
    // 验证：即使投递 20 个任务，同时执行的也不超过 worker 数
    ThreadPool pool(3);
    std::atomic<int> max_concurrent{0};
    std::atomic<int> current{0};
    std::atomic<int> finished{0};

    for (int i = 0; i < 20; ++i) {
        pool.enqueue([&]() {
            int c = current.fetch_add(1, std::memory_order_relaxed) + 1;
            int prev = max_concurrent.load();
            while (c > prev) {
                if (max_concurrent.compare_exchange_weak(prev, c)) break;
            }
            std::this_thread::sleep_for(10ms);
            current.fetch_sub(1, std::memory_order_relaxed);
            finished.fetch_add(1, std::memory_order_relaxed);
        });
    }

    // 轮询终态：20 个任务全部跑完（不能等 current==0 —— 任务之间也会瞬时归零，
    // 提前退出会让 max_concurrent 观察值偏小，「至少发生过并发」断言随之抖动）
    wait_for([&] { return finished.load() == 20; }, 10s, "20 个任务全部跑完");

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
    wait_for([&] { return pool.active_count() > 0; }, 5s, "worker 已接管长任务");

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
        // 轮询终态等待任务完成
        wait_for([&] { return counter.load() == 5; }, 5s, "5 个任务全部执行完");
    }  // 析构
    REQUIRE(counter.load() == 5);
}

TEST_CASE("ThreadPool handles empty task gracefully", "[thread_pool][basic]") {
    ThreadPool pool(1);
    pool.enqueue(nullptr);  // 不应崩溃
    // nullptr 任务在 enqueue 时即被丢弃，队列应立刻为空（有超时上限，不靠 sleep 猜）
    wait_for([&] { return pool.pending_count() == 0; }, 2s, "空任务已跳过");
    REQUIRE(pool.pending_count() == 0);
}
