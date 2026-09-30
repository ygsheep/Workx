/**
 * @file task_manager.cpp
 * @brief 异步任务管理器实现
 * @details 基于 ThreadPool 替代裸 std::thread，限制并发线程数
 */

#include "core/task/task_manager.h"
#include "core/task/task_events.h"
#include "liblogger/logger.h"
#include <algorithm>

namespace agent {

Task::Task(std::string name, TaskFunc func, IEventBus& event_bus, FinishedCallback on_finished,
           float max_progress)
    : m_name(std::move(name)),
      m_func(std::move(func)),
      m_max_progress(max_progress),
      m_event_bus(event_bus),
      m_on_finished(std::move(on_finished)) {}

Task::~Task() = default;

void Task::append_output(const std::string& line) {
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        m_output += line;
        m_output += '\n';
    }
    // #26 评审 #6：TaskOutputEvent 仅传 task_name + line，不持有 shared_ptr<Task>，
    // 避免经 event_bus 异步发布时延长任务对象（及输出缓冲）生命周期
    m_event_bus.publish_async(TaskOutputEvent{.task_name = m_name, .line = line});
}

std::string Task::output() const {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    return m_output;
}

void Task::execute() {
    // CAS：仅当处于 Pending 时才进入 Running，避免重复 execute
    TaskStatus expected = TaskStatus::Pending;
    if (!m_status.compare_exchange_strong(expected, TaskStatus::Running, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
        return;
    }

    m_start_time_ns.store(std::chrono::steady_clock::now().time_since_epoch().count(),
                          std::memory_order_relaxed);
    LOG_DEBUG("[task name={}] execute begin", m_name);

    const auto task_ptr = shared_from_this();

    m_event_bus.publish_async(TaskStartedEvent{.task = task_ptr, .task_name = m_name});

    try {
        m_func(m_should_cancel);

        if (!m_should_cancel.load(std::memory_order_acquire)) {
            markCompleted();
        } else {
            m_status.store(TaskStatus::Cancelled, std::memory_order_release);

            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start_time())
                                .count();

            LOG_INFO("[task name={}] cancelled, duration={}ms", m_name, duration);

            m_event_bus.publish_async(
                TaskCancelledEvent{.task = task_ptr,
                                   .task_name = m_name,
                                   .duration_ms = static_cast<float>(duration)});
        }
    } catch (const std::exception& e) {
        LOG_ERROR("[task name={}] unhandled exception: {}", m_name, e.what());
        markFailed(e.what());
    } catch (...) {
        LOG_ERROR("[task name={}] unhandled unknown exception", m_name);
        markFailed("Unknown exception");
    }

    // 通知 TaskManager 任务已结束，唤醒 waitForAll
    // （线程池模式下任务在 worker 线程执行，无 thread 可 join）
    if (m_on_finished) {
        m_on_finished();
    }
}

void Task::markCompleted() {
    m_progress.store(m_max_progress.load(std::memory_order_relaxed), std::memory_order_relaxed);
    m_status.store(TaskStatus::Completed, std::memory_order_release);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time())
                        .count();

    LOG_DEBUG("[task name={}] execute end, status=Completed, duration={}ms", m_name, duration);

    auto task_ptr = shared_from_this();

    m_event_bus.publish_async(TaskCompletedEvent{
        .task = task_ptr, .task_name = m_name, .duration_ms = static_cast<float>(duration)});

    if (m_completed_callback) {
        m_completed_callback();
    }
}

void Task::markFailed(const std::string& error_message) {
    m_status.store(TaskStatus::Failed, std::memory_order_release);

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start_time())
                        .count();

    LOG_DEBUG("[task name={}] execute end, status=Failed, duration={}ms, error={}", m_name,
              duration, error_message);

    auto task_ptr = shared_from_this();

    m_event_bus.publish_async(TaskFailedEvent{.task = task_ptr,
                                              .task_name = m_name,
                                              .error_message = error_message,
                                              .duration_ms = static_cast<float>(duration)});
}

TaskManager::~TaskManager() {
    // 先取消所有任务，再等待排空，最后 ThreadPool 析构时 shutdown 会 join 工作线程
    cancelAll();
    waitForAll();
    // ThreadPool m_pool 析构自动 shutdown
}

std::shared_ptr<Task> TaskManager::create(const std::string& name, Task::TaskFunc func,
                                          TaskType type) {
    // 捕获 this 通过 callback 通知 m_tasks_cv / m_wait_cv，避免 Task 反向依赖 TaskManager 单例
    // H-D：m_wait_cv 与 m_tasks_cv 分离，wait(task) 不再持 m_tasks_mutex，避免与 start/cancel 死锁
    auto task = std::make_shared<Task>(
        name, std::move(func), m_event_bus,
        [this]() {
            m_tasks_cv.notify_all();
            m_wait_cv.notify_all();
        },
        100.0f);
    task->setType(type);
    return task;
}

std::shared_ptr<Task> TaskManager::launch(const std::string& name, Task::TaskFunc func,
                                          TaskType type) {
    auto task = create(name, std::move(func), type);
    start(task);
    return task;
}

void TaskManager::start(std::shared_ptr<Task> task) {
    if (!task) return;

    if (task->getType() == TaskType::Blocking) {
        LOG_INFO("[task name={}] started (blocking, sync)", task->getName());
        task->execute();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_tasks_mutex);
        m_entries.push_back(task);
    }

    LOG_INFO("[task name={}] enqueued, pool_workers={}, pending={}", task->getName(),
             m_pool.worker_count(), m_pool.pending_count() + 1);

    // 2.2：投递到线程池，由 worker 线程消费
    // 捕获 task 保持引用计数，防止任务执行前 task 被销毁
    m_pool.enqueue([task]() { task->execute(); });
}

void TaskManager::cancel(std::shared_ptr<Task> task) {
    if (!task) return;
    LOG_INFO("[task name={}] cancel requested", task->getName());
    task->cancel();
}

std::vector<std::shared_ptr<Task>> TaskManager::getTasks() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    return m_entries;
}

std::vector<std::shared_ptr<Task>> TaskManager::getRunningTasks() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    std::vector<std::shared_ptr<Task>> running;
    for (const auto& task : m_entries) {
        if (task->isRunning()) {
            running.push_back(task);
        }
    }
    return running;
}

void TaskManager::update() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);

    size_t cleaned = 0;
    for (auto it = m_entries.begin(); it != m_entries.end();) {
        // Critical 类型不清理（持久任务）
        if ((*it)->isFinished() && (*it)->getType() != TaskType::Critical) {
            it = m_entries.erase(it);
            ++cleaned;
        } else {
            ++it;
        }
    }
    if (cleaned > 0) {
        LOG_DEBUG("cleanup {} finished tasks, remaining={}, pool_pending={}", cleaned,
                  m_entries.size(), m_pool.pending_count());
    }
}

void TaskManager::waitForAll() {
    std::unique_lock<std::mutex> lock(m_tasks_mutex);
    // 30 秒兜底，防止 task 不响应 cancel 而永久阻塞
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    m_tasks_cv.wait_until(lock, deadline, [this]() {
        return m_entries.empty() ||
               std::all_of(m_entries.begin(), m_entries.end(),
                           [](const std::shared_ptr<Task>& t) { return t->isFinished(); });
    });
}

void TaskManager::wait(std::shared_ptr<Task> task) {
    if (!task) return;
    // H-9：替代 ChatSession 析构中的 sleep_for(50ms) 轮询
    // H-D：不持有 m_tasks_mutex，避免与 start()/cancel()/getTasks() 等需要 m_tasks_mutex
    //      的路径死锁。利用 Task::execute 结束时调用的 m_on_finished 回调通知 m_wait_cv。
    std::unique_lock<std::mutex> lock(m_wait_mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    m_wait_cv.wait_until(lock, deadline, [&task]() { return task->isFinished(); });
}

void TaskManager::waitForTasks(const std::vector<std::shared_ptr<Task>>& tasks) {
    if (tasks.empty()) return;
    // M-2：并行等待一批任务，单一 30s 兜底超时（替代逐个 wait 的最坏 N×30s）。
    // 任一任务结束时 m_on_finished 通知 m_wait_cv，重新检查谓词。
    std::unique_lock<std::mutex> lock(m_wait_mutex);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    m_wait_cv.wait_until(lock, deadline, [&tasks]() {
        return std::all_of(tasks.begin(), tasks.end(),
                           [](const std::shared_ptr<Task>& t) { return t->isFinished(); });
    });
}

void TaskManager::cancelAll() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    for (auto& task : m_entries) {
        if (task->isRunning()) {
            task->cancel();
        }
    }
}

size_t TaskManager::getRunningTaskCount() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    return std::count_if(m_entries.begin(), m_entries.end(),
                         [](const std::shared_ptr<Task>& t) { return t->isRunning(); });
}

std::shared_ptr<Task> TaskManager::find_task(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    const auto it =
        std::find_if(m_entries.begin(), m_entries.end(),
                     [&name](const std::shared_ptr<Task>& t) { return t->getName() == name; });
    return it == m_entries.end() ? nullptr : *it;
}

}  // namespace agent
