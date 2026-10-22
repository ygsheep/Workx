/**
 * @file mock_task_manager.h
 * @brief 测试用 Mock ITaskManager
 * @details 记录所有任务创建/启动/取消操作，不实际执行任务函数（除非显式启用）。
 *          供需要注入 ITaskManager 的组件（Terminal/Client/ChatSession 等）做隔离测试。
 *
 * 使用示例：
 * @code
 *   using namespace agent::test;
 *   MockTaskManager tm;
 *   auto task = tm.launch("test", [](const std::atomic<bool>&) {});
 *   REQUIRE(tm.launched_count() == 1);
 * @endcode
 */

#pragma once

#include <atomic>
#include <algorithm>
#include <mutex>
#include <vector>
#include <memory>
#include <string>
#include <functional>

#include "core/task/task_manager.h"

namespace agent::test {

/// @brief Mock ITaskManager
/// @details 线程安全（内部互斥锁）。create/launch 返回真实 Task 对象但不执行
///          任务函数；调用 launch 时若 set_execute_enabled(true) 则同步执行。
class MockTaskManager final : public ITaskManager {
   public:
    MockTaskManager() = default;
    ~MockTaskManager() override = default;

    MockTaskManager(const MockTaskManager&) = delete;
    MockTaskManager& operator=(const MockTaskManager&) = delete;

    // === ITaskManager 实现 ===

    std::shared_ptr<Task> create(const std::string& name, Task::TaskFunc func,
                                 TaskType type = TaskType::Normal) override {
        auto task =
            std::make_shared<Task>(name, std::move(func), EventBus::instance(), nullptr, 100.0f);
        task->setType(type);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.push_back(task);
        ++m_create_count;
        return task;
    }

    std::shared_ptr<Task> launch(const std::string& name, Task::TaskFunc func,
                                 TaskType type = TaskType::Normal) override {
        auto task = create(name, std::move(func), type);
        start(task);
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_launch_count;
        return task;
    }

    void start(std::shared_ptr<Task> task) override {
        if (!task) return;
        // Mock 实现：不实际执行任务函数，仅记录 start 调用
        // 真实执行需要 TaskManager 的 ThreadPool 支持，Mock 场景通常不需要
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_start_count;
    }

    void cancel(std::shared_ptr<Task> task) override {
        if (!task) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_count;
        // 标记任务为已取消（通过 Task 的接口）
        // Task 没有 cancel 方法，但 setStatus 在 task_manager.cpp 内部使用
        // 这里仅记录调用，不修改 task 状态
    }

    [[nodiscard]] std::vector<std::shared_ptr<Task>> getTasks() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks;
    }

    [[nodiscard]] std::vector<std::shared_ptr<Task>> getRunningTasks() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::shared_ptr<Task>> running;
        for (const auto& t : m_tasks) {
            if (t->getStatus() == TaskStatus::Running) {
                running.push_back(t);
            }
        }
        return running;
    }

    [[nodiscard]] size_t getRunningTaskCount() const override { return getRunningTasks().size(); }

    [[nodiscard]] std::shared_ptr<Task> find_task(const std::string& name) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it =
            std::find_if(m_tasks.begin(), m_tasks.end(),
                         [&name](const std::shared_ptr<Task>& t) { return t->getName() == name; });
        return it == m_tasks.end() ? nullptr : *it;
    }

    void update() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_update_count;
    }

    void waitForAll() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_wait_count;
    }

    void cancelAll() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_all_count;
    }

    void wait(std::shared_ptr<Task> /*task*/) override {
        // H-9：Mock 不实际执行任务，wait 立即返回
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_wait_task_count;
    }

    void waitForTasks(const std::vector<std::shared_ptr<Task>>& /*tasks*/) override {
        // M-2：Mock 不实际执行任务，waitForTasks 立即返回
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_wait_tasks_count;
    }

    // === 测试辅助 API ===

    /// @brief create 调用次数
    [[nodiscard]] size_t create_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_create_count;
    }

    /// @brief launch 调用次数
    [[nodiscard]] size_t launched_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_launch_count;
    }

    /// @brief start 调用次数
    [[nodiscard]] size_t start_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_start_count;
    }

    /// @brief cancel 调用次数
    [[nodiscard]] size_t cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

    /// @brief update 调用次数
    [[nodiscard]] size_t update_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_update_count;
    }

    /// @brief waitForAll 调用次数
    [[nodiscard]] size_t wait_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_wait_count;
    }

    /// @brief cancelAll 调用次数
    [[nodiscard]] size_t cancel_all_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_all_count;
    }

    /// @brief wait(task) 调用次数（H-9）
    [[nodiscard]] size_t wait_task_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_wait_task_count;
    }

    /// @brief waitForTasks 调用次数（M-2）
    [[nodiscard]] size_t wait_tasks_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_wait_tasks_count;
    }

    /// @brief 清空所有记录
    void clear_history() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.clear();
        m_create_count = 0;
        m_launch_count = 0;
        m_start_count = 0;
        m_cancel_count = 0;
        m_update_count = 0;
        m_wait_count = 0;
        m_cancel_all_count = 0;
        m_wait_task_count = 0;
        m_wait_tasks_count = 0;
    }

   private:
    mutable std::mutex m_mutex;
    std::vector<std::shared_ptr<Task>> m_tasks;
    size_t m_create_count = 0;
    size_t m_launch_count = 0;
    size_t m_start_count = 0;
    size_t m_cancel_count = 0;
    size_t m_update_count = 0;
    size_t m_wait_count = 0;
    size_t m_cancel_all_count = 0;
    size_t m_wait_task_count = 0;
    size_t m_wait_tasks_count = 0;
};

}  // namespace agent::test
