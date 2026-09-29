/**
 * @file test_task_manager.cpp
 * @brief TaskManager 单元测试
 * @details 覆盖 Task 生命周期、TaskManager create/launch/start/cancel/waitForAll/cancelAll
 *          以及 update() 清理、TaskType::Blocking 同步执行、异常捕获、并发任务等场景
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#include "core/task/task_manager.h"
#include "core/events/event_bus.h"

using namespace agent;
using namespace std::chrono_literals;

namespace {

/// @brief 每个测试前清理单例残留
struct TaskManagerFixture {
    TaskManagerFixture() {
        // 清理 EventBus 残留订阅与异步事件
        EventBus::instance().clear();
        // 取消所有遗留任务并等待
        TaskManager::instance().cancelAll();
        TaskManager::instance().waitForAll();
        TaskManager::instance().update();
    }
    ~TaskManagerFixture() {
        TaskManager::instance().cancelAll();
        TaskManager::instance().waitForAll();
        TaskManager::instance().update();
        EventBus::instance().clear();
    }
};

}  // namespace

// ============================================================================
// Task basic behavior
// ============================================================================

TEST_CASE("Task default status is Pending", "[task_manager][task]") {
    Task task("test", [](const std::atomic<bool>&) {}, EventBus::instance());
    REQUIRE(task.getStatus() == TaskStatus::Pending);
    REQUIRE(task.getName() == "test");
    REQUIRE(task.getType() == TaskType::Normal);
    REQUIRE_FALSE(task.isRunning());
    REQUIRE_FALSE(task.isFinished());
}

TEST_CASE("Task setProgress updates progress and completes at max", "[task_manager][task]") {
    Task task("progress_test", [](const std::atomic<bool>&) {}, EventBus::instance(), {}, 100.0f);
    REQUIRE(task.getProgress() == 0.0f);
    REQUIRE(task.getMaxProgress() == 100.0f);

    task.setProgress(50.0f);
    REQUIRE(task.getProgress() == 50.0f);
    REQUIRE(task.getStatus() == TaskStatus::Pending);  // not yet completed

    task.setProgress(100.0f);
    REQUIRE(task.getProgress() == 100.0f);
    REQUIRE(task.getStatus() == TaskStatus::Completed);
    REQUIRE(task.isFinished());
}

TEST_CASE("Task setProgress clamps to max_progress", "[task_manager][task]") {
    Task task("clamp", [](const std::atomic<bool>&) {}, EventBus::instance(), {}, 50.0f);
    task.setProgress(200.0f);
    REQUIRE(task.getProgress() == 50.0f);
    REQUIRE(task.getStatus() == TaskStatus::Completed);
}

TEST_CASE("Task addProgress increments and completes at max", "[task_manager][task]") {
    Task task("incremental", [](const std::atomic<bool>&) {}, EventBus::instance(), {}, 30.0f);

    task.addProgress(10.0f);
    REQUIRE(task.getProgress() == 10.0f);

    task.addProgress(15.0f);
    REQUIRE(task.getProgress() == 25.0f);
    REQUIRE(task.getStatus() == TaskStatus::Pending);

    task.addProgress(10.0f);  // would exceed 30
    REQUIRE(task.getProgress() == 30.0f);
    REQUIRE(task.getStatus() == TaskStatus::Completed);
}

TEST_CASE("Task cancel sets should_cancel flag", "[task_manager][task]") {
    Task task("cancellable", [](const std::atomic<bool>&) {}, EventBus::instance());
    REQUIRE_FALSE(task.shouldCancel());

    task.cancel();
    REQUIRE(task.shouldCancel());
}

TEST_CASE("Task setType changes type", "[task_manager][task]") {
    Task task("typed", [](const std::atomic<bool>&) {}, EventBus::instance());
    REQUIRE(task.getType() == TaskType::Normal);

    task.setType(TaskType::Background);
    REQUIRE(task.getType() == TaskType::Background);

    task.setType(TaskType::Critical);
    REQUIRE(task.getType() == TaskType::Critical);
}

// ============================================================================
// TaskManager create & launch
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager create returns task without starting",
                 "[task_manager][create]") {
    bool executed = false;
    auto task = TaskManager::instance().create(
        "not_started", [&executed](const std::atomic<bool>&) { executed = true; });

    REQUIRE(task != nullptr);
    REQUIRE(task->getName() == "not_started");
    REQUIRE(task->getStatus() == TaskStatus::Pending);
    REQUIRE_FALSE(executed);
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager launch starts task", "[task_manager][launch]") {
    std::atomic<bool> executed{false};
    auto task = TaskManager::instance().launch(
        "launched", [&executed](const std::atomic<bool>&) { executed = true; });

    // wait for completion
    TaskManager::instance().waitForAll();
    REQUIRE(executed.load());
    REQUIRE(task->isFinished());
    REQUIRE(task->getStatus() == TaskStatus::Completed);
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager wait(task) blocks until finished (H-9)",
                 "[task_manager][wait][h9]") {
    // H-9：验证 wait(task) 替代 sleep_for 轮询
    std::atomic<bool> executed{false};
    auto task =
        TaskManager::instance().launch("wait-target", [&executed](const std::atomic<bool>&) {
            std::this_thread::sleep_for(50ms);
            executed = true;
        });

    // wait(task) 应阻塞直到 task 结束
    TaskManager::instance().wait(task);
    REQUIRE(executed.load());
    REQUIRE(task->isFinished());
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager wait(nullptr) is no-op (H-9)",
                 "[task_manager][wait][h9]") {
    // H-9：nullptr 安全性
    REQUIRE_NOTHROW(TaskManager::instance().wait(nullptr));
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager launch Blocking task executes synchronously",
                 "[task_manager][blocking]") {
    std::atomic<bool> executed{false};
    auto task = TaskManager::instance().launch(
        "blocking", [&executed](const std::atomic<bool>&) { executed = true; }, TaskType::Blocking);

    // Blocking 任务应在 launch 返回前已执行完
    REQUIRE(executed.load());
    REQUIRE(task->getStatus() == TaskStatus::Completed);
}

// ============================================================================
// Task cancellation
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture, "Task cancellation via should_cancel flag",
                 "[task_manager][cancel]") {
    std::atomic<bool> started{false};
    std::atomic<bool> cancelled_observed{false};

    auto task = TaskManager::instance().launch(
        "cancellable_long",
        [&started, &cancelled_observed](const std::atomic<bool>& should_cancel) {
            started = true;
            // 模拟长任务，循环检查取消信号
            for (int i = 0; i < 100; ++i) {
                if (should_cancel) {
                    cancelled_observed = true;
                    return;
                }
                std::this_thread::sleep_for(2ms);
            }
        });

    // 等任务开始
    while (!started.load()) std::this_thread::sleep_for(1ms);

    task->cancel();
    TaskManager::instance().waitForAll();

    REQUIRE(cancelled_observed.load());
    REQUIRE(task->getStatus() == TaskStatus::Cancelled);
    REQUIRE(task->isFinished());
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager cancelAll cancels all running tasks",
                 "[task_manager][cancel_all]") {
    constexpr int TASK_COUNT = 3;
    std::atomic<int> cancelled_count{0};
    std::vector<std::shared_ptr<Task>> tasks;

    for (int i = 0; i < TASK_COUNT; ++i) {
        auto task = TaskManager::instance().launch(
            "task_" + std::to_string(i),
            [&cancelled_count](const std::atomic<bool>& should_cancel) {
                while (!should_cancel) {
                    std::this_thread::sleep_for(5ms);
                }
                cancelled_count++;
            });
        tasks.push_back(task);
    }

    // 确保任务已经开始
    std::this_thread::sleep_for(20ms);

    TaskManager::instance().cancelAll();
    TaskManager::instance().waitForAll();

    REQUIRE(cancelled_count.load() == TASK_COUNT);
    for (const auto& task : tasks) {
        REQUIRE(task->getStatus() == TaskStatus::Cancelled);
    }
}

// ============================================================================
// Task exception handling
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture, "Task throwing std::exception marks Failed",
                 "[task_manager][exception]") {
    auto task = TaskManager::instance().launch("throws_std", [](const std::atomic<bool>&) {
        throw std::runtime_error("intentional failure");
    });

    TaskManager::instance().waitForAll();

    REQUIRE(task->getStatus() == TaskStatus::Failed);
    REQUIRE(task->isFinished());
}

TEST_CASE_METHOD(TaskManagerFixture, "Task throwing unknown exception marks Failed",
                 "[task_manager][exception]") {
    auto task = TaskManager::instance().launch("throws_unknown", [](const std::atomic<bool>&) {
        throw 42;  // non-std exception
    });

    TaskManager::instance().waitForAll();

    REQUIRE(task->getStatus() == TaskStatus::Failed);
    REQUIRE(task->isFinished());
}

// ============================================================================
// update() cleanup
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager update cleans up finished tasks",
                 "[task_manager][update]") {
    // 启动 3 个快速完成的任务
    for (int i = 0; i < 3; ++i) {
        TaskManager::instance().launch("quick_" + std::to_string(i),
                                       [](const std::atomic<bool>&) { /* immediate completion */ });
    }

    TaskManager::instance().waitForAll();

    // update 前可能有 finished 任务残留
    size_t before = TaskManager::instance().getTasks().size();
    REQUIRE(before >= 3);

    TaskManager::instance().update();

    // update 后应清理 finished 任务
    size_t after = TaskManager::instance().getTasks().size();
    REQUIRE(after == 0);
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager update does not clean Critical tasks",
                 "[task_manager][update]") {
    std::atomic<bool> started{false};
    auto task = TaskManager::instance().create("critical",
                                               [&started](const std::atomic<bool>& should_cancel) {
                                                   started = true;
                                                   while (!should_cancel) {
                                                       std::this_thread::sleep_for(2ms);
                                                   }
                                               });
    task->setType(TaskType::Critical);
    TaskManager::instance().start(task);

    while (!started.load()) std::this_thread::sleep_for(1ms);
    task->cancel();
    TaskManager::instance().waitForAll();

    // Critical 任务即使 finished 也不应被 update 清理
    REQUIRE(task->isFinished());
    TaskManager::instance().update();
    REQUIRE(TaskManager::instance().getTasks().size() == 1);

    // 恢复 Normal 类型，让 fixture 析构时能清理（Critical 不被 update 清理）
    task->setType(TaskType::Normal);
    TaskManager::instance().update();
    REQUIRE(TaskManager::instance().getTasks().size() == 0);
}

// ============================================================================
// Concurrency
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager concurrent launch executes all tasks",
                 "[task_manager][concurrency]") {
    constexpr int TASK_COUNT = 10;
    std::atomic<int> completed_count{0};

    std::vector<std::shared_ptr<Task>> tasks;
    for (int i = 0; i < TASK_COUNT; ++i) {
        auto task = TaskManager::instance().launch("concurrent_" + std::to_string(i),
                                                   [&completed_count](const std::atomic<bool>&) {
                                                       std::this_thread::sleep_for(5ms);
                                                       completed_count++;
                                                   });
        tasks.push_back(task);
    }

    TaskManager::instance().waitForAll();

    REQUIRE(completed_count.load() == TASK_COUNT);
    for (const auto& task : tasks) {
        REQUIRE(task->getStatus() == TaskStatus::Completed);
    }
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager getRunningTasks returns only running",
                 "[task_manager][query]") {
    std::atomic<bool> long_started{false};
    auto long_task = TaskManager::instance().launch(
        "long_running", [&long_started](const std::atomic<bool>& should_cancel) {
            long_started = true;
            while (!should_cancel) std::this_thread::sleep_for(2ms);
        });

    auto quick_task = TaskManager::instance().launch("quick", [](const std::atomic<bool>&) {});

    // 等待 quick 完成，long 启动
    while (!long_started.load() || !quick_task->isFinished()) {
        std::this_thread::sleep_for(1ms);
    }

    auto running = TaskManager::instance().getRunningTasks();
    REQUIRE(running.size() == 1);
    REQUIRE(running[0]->getName() == "long_running");

    TaskManager::instance().cancelAll();
    TaskManager::instance().waitForAll();
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager getRunningTaskCount returns correct count",
                 "[task_manager][query]") {
    std::atomic<bool> started{false};
    std::atomic<int> active{0};

    auto t1 = TaskManager::instance().launch(
        "r1", [&started, &active](const std::atomic<bool>& should_cancel) {
            started = true;
            active++;
            while (!should_cancel) std::this_thread::sleep_for(2ms);
            active--;
        });
    auto t2 =
        TaskManager::instance().launch("r2", [&active](const std::atomic<bool>& should_cancel) {
            active++;
            while (!should_cancel) std::this_thread::sleep_for(2ms);
            active--;
        });

    while (!started.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(20ms);  // 让两个任务都进入运行

    REQUIRE(TaskManager::instance().getRunningTaskCount() >= 1);

    TaskManager::instance().cancelAll();
    TaskManager::instance().waitForAll();
    REQUIRE(TaskManager::instance().getRunningTaskCount() == 0);
}

// ============================================================================
// onCompleted callback
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture, "Task onCompleted callback fires on completion",
                 "[task_manager][callback][slow]") {
    std::atomic<bool> callback_fired{false};

    auto task = std::make_shared<Task>(
        "with_callback", [](const std::atomic<bool>&) { /* quick */ }, EventBus::instance(),
        std::function<void()>{}, 100.0f);
    task->onCompleted([&callback_fired]() { callback_fired = true; });

    TaskManager::instance().start(task);
    TaskManager::instance().waitForAll();

    REQUIRE(task->getStatus() == TaskStatus::Completed);
    REQUIRE(callback_fired.load());
}

// ============================================================================
// 压力测试：并发启动大量 Task，验证线程数受 ThreadPool 限制
// ============================================================================

TEST_CASE_METHOD(TaskManagerFixture,
                 "TaskManager stress test: 50 concurrent tasks limited by thread pool",
                 "[task_manager][stress][slow]") {
    // TaskManager 单例的线程池大小 = hardware_concurrency（测试环境通常 4-16）
    const size_t pool_workers = TaskManager::instance().worker_count();
    REQUIRE(pool_workers >= 1);

    std::atomic<int> concurrent{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<int> completed_count{0};

    constexpr int TASK_COUNT = 50;
    std::atomic<bool> release{false};

    for (int i = 0; i < TASK_COUNT; ++i) {
        auto task = std::make_shared<Task>(
            "stress_" + std::to_string(i),
            [&](const std::atomic<bool>&) {
                int c = concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
                // CAS 更新 max_concurrent
                int prev = max_concurrent.load(std::memory_order_relaxed);
                while (c > prev) {
                    if (max_concurrent.compare_exchange_weak(prev, c, std::memory_order_relaxed,
                                                             std::memory_order_relaxed))
                        break;
                }
                // 阻塞直到 release
                while (!release.load(std::memory_order_relaxed)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                concurrent.fetch_sub(1, std::memory_order_relaxed);
                completed_count.fetch_add(1, std::memory_order_relaxed);
            },
            EventBus::instance(), std::function<void()>{}, 100.0f);
        TaskManager::instance().start(task);
    }

    // 等待一段时间让任务排满线程池
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 核心断言：并发数不超过线程池大小
    REQUIRE(max_concurrent.load() <= static_cast<int>(pool_workers));
    REQUIRE(max_concurrent.load() >= 1);  // 至少有一个在运行

    // 释放所有任务
    release.store(true);

    TaskManager::instance().waitForAll();
    REQUIRE(completed_count.load() == TASK_COUNT);

    // update 清理后应无残留
    TaskManager::instance().update();
    REQUIRE(TaskManager::instance().getTasks().empty());
}

TEST_CASE_METHOD(TaskManagerFixture, "TaskManager pool rejects unbounded thread growth",
                 "[task_manager][stress]") {
    // 验证：即使投递 100 个任务，工作线程数仍固定为初始值
    const size_t initial_workers = TaskManager::instance().worker_count();

    std::atomic<int> done{0};
    for (int i = 0; i < 100; ++i) {
        TaskManager::instance().launch(
            "burst_" + std::to_string(i), [&done](const std::atomic<bool>&) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                done.fetch_add(1, std::memory_order_relaxed);
            });
    }

    // 工作线程数不应增长
    REQUIRE(TaskManager::instance().worker_count() == initial_workers);

    TaskManager::instance().waitForAll();
    REQUIRE(done.load() == 100);
}

// ============================================================================
// M-6: Task 状态机可独立测试（friend 已删除，execute() 公开）
// ============================================================================

TEST_CASE("Task execute is public, drives state machine without friend (M-6)",
          "[task_manager][task][m6]") {
    // M-6：验证 Task::execute 已为 public，测试可直接调用驱动状态机，
    //      无需 friend class TaskManager 反向访问私有成员。
    //      Task 继承 enable_shared_from_this，execute 内部调用 shared_from_this()，
    //      因此必须用 shared_ptr 构造（不能栈对象）。
    std::atomic<bool> executed{false};
    auto task = std::make_shared<Task>(
        "m6_direct_execute", [&executed](const std::atomic<bool>&) { executed = true; },
        EventBus::instance());

    REQUIRE(task->getStatus() == TaskStatus::Pending);
    REQUIRE_FALSE(executed.load());

    // 直接调用 public execute()，不经过 TaskManager::start
    task->execute();

    REQUIRE(executed.load());
    REQUIRE(task->getStatus() == TaskStatus::Completed);
    REQUIRE(task->isFinished());

    // 二次 execute 应为 no-op（CAS 保护：Pending → Running 仅一次）
    task->execute();
    REQUIRE(task->getStatus() == TaskStatus::Completed);
}

TEST_CASE("Task execute transitions Pending → Running → Cancelled (M-6)",
          "[task_manager][task][m6]") {
    // M-6：验证 execute 在 should_cancel=true 时走 Cancelled 分支
    auto task = std::make_shared<Task>(
        "m6_cancelled", [](const std::atomic<bool>&) { /* 用户函数检查 cancel 并返回 */ },
        EventBus::instance());
    task->cancel();  // 设置 should_cancel

    REQUIRE(task->shouldCancel());
    REQUIRE(task->getStatus() == TaskStatus::Pending);

    task->execute();

    REQUIRE(task->getStatus() == TaskStatus::Cancelled);
    REQUIRE(task->isFinished());
}

TEST_CASE("Task execute catches exception and marks Failed (M-6)", "[task_manager][task][m6]") {
    // M-6：验证 execute 异常路径（markFailed 为 private，通过 execute 间接触发）
    auto task = std::make_shared<Task>(
        "m6_throws", [](const std::atomic<bool>&) { throw std::runtime_error("boom"); },
        EventBus::instance());

    task->execute();

    REQUIRE(task->getStatus() == TaskStatus::Failed);
    REQUIRE(task->isFinished());
}
