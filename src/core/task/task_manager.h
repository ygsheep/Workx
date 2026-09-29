/**
 * @file task_manager.h
 * @brief 异步任务管理器
 * @details 管理任务执行、进度跟踪和协作式取消
 *          - 使用 ThreadPool 替代裸 std::thread（2.2）
 *          - 抽取 ITaskManager 接口支持 DI（D-1）
 * @version 2.0.0
 */

#pragma once

#include <functional>
#include <string>
#include <memory>

#include "core/export.h"
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <type_traits>

#include "core/task/thread_pool.h"
#include "core/events/event_bus.h"

namespace agent {

enum class TaskType { Normal, Background, Blocking, Critical };

enum class TaskStatus { Pending, Running, Completed, Cancelled, Failed };

class TaskManager;

class Task : public std::enable_shared_from_this<Task> {
   public:
    using TaskFunc = std::function<void(const std::atomic<bool>& should_cancel)>;
    using FinishedCallback = std::function<void()>;

    // T-6：编译期验证 start_time 原子字段满足 trivially copyable 要求
    // 注意：MSVC 对 std::atomic<T> 的 is_trivially_copyable 实现有历史 bug，
    //       改为验证底层类型 int64_t 满足 trivially copyable（标准保证 atomic<T> 与 T 同为
    //       trivially copyable）
    static_assert(std::is_trivially_copyable_v<int64_t>,
                  "int64_t must be trivially copyable (atomic<int64_t> shares this property)");

    /// @brief 构造
    /// @param event_bus 事件总线引用（D-1 DI：Task 通过它发布生命周期事件）
    /// @param on_finished 任务结束时的通知回调（用于唤醒 TaskManager::waitForAll）
    Task(std::string name, TaskFunc func, IEventBus& event_bus, FinishedCallback on_finished = {},
         float max_progress = 100.0f);
    ~Task();

    [[nodiscard]] const std::string& getName() const { return m_name; }
    [[nodiscard]] TaskType getType() const { return m_type; }
    void setType(const TaskType& type) { m_type = type; }
    /// @brief 获取任务状态（线程安全，原子读取）
    [[nodiscard]] TaskStatus getStatus() const { return m_status.load(std::memory_order_acquire); }

    /// @brief 获取当前进度（线程安全，原子读取）
    [[nodiscard]] float getProgress() const { return m_progress.load(std::memory_order_relaxed); }
    /// @brief 获取进度上限（线程安全，原子读取）
    [[nodiscard]] float getMaxProgress() const {
        return m_max_progress.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getProgressPercent() const {
        const float max = m_max_progress.load(std::memory_order_relaxed);
        if (max <= 0) return 0;
        return m_progress.load(std::memory_order_relaxed) / max;
    }

    /// @brief 设置进度（线程安全）。达到上限时自动标记 Completed
    void setProgress(float progress) {
        const float max = m_max_progress.load(std::memory_order_relaxed);
        const float clamped = std::min(progress, max);
        m_progress.store(clamped, std::memory_order_relaxed);
        if (clamped >= max) {
            m_status.store(TaskStatus::Completed, std::memory_order_release);
        }
    }

    /// @brief 增量更新进度（线程安全）
    void addProgress(float delta) {
        const float max = m_max_progress.load(std::memory_order_relaxed);
        // CAS 循环：保证并发 addProgress 不丢失更新
        float current = m_progress.load(std::memory_order_relaxed);
        float next;
        do {
            next = std::min(current + delta, max);
        } while (!m_progress.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                                   std::memory_order_relaxed));
        if (next >= max) {
            m_status.store(TaskStatus::Completed, std::memory_order_release);
        }
    }

    // 仅设置取消请求标志，不立即修改 status；由 execute() 检测后置 Cancelled
    void cancel() { m_should_cancel.store(true, std::memory_order_release); }

    [[nodiscard]] bool shouldCancel() const {
        return m_should_cancel.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isFinished() const {
        const auto s = m_status.load(std::memory_order_acquire);
        return s == TaskStatus::Completed || s == TaskStatus::Cancelled || s == TaskStatus::Failed;
    }

    [[nodiscard]] bool isRunning() const {
        return m_status.load(std::memory_order_acquire) == TaskStatus::Running;
    }

    void onCompleted(std::function<void()> callback) { m_completed_callback = std::move(callback); }

    /// @brief 追加输出行（线程安全）
    /// @details 累加到内部缓冲并发布 TaskOutputEvent，供 TaskOutputTool / UI 读取。
    ///          事件经 m_event_bus 异步发布（与 TaskStartedEvent 等生命周期事件一致）。
    void append_output(const std::string& line);

    /// @brief 读取累计输出（线程安全，返回拷贝）
    [[nodiscard]] std::string output() const;

    /// @brief 执行任务（M-6：原 private + friend TaskManager，改为 public）
    /// @details TaskManager::start 通过此入口驱动任务执行；
    ///          测试可直接调用 execute() 驱动状态机，无需 friend。
    ///          状态转换：Pending → Running → (Completed|Cancelled|Failed)
    void execute();

    /// @brief 读取开始时间点（线程安全）
    [[nodiscard]] std::chrono::steady_clock::time_point start_time() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::nanoseconds{m_start_time_ns.load(std::memory_order_relaxed)}};
    }

   private:
    // M-6：状态机内部转换方法保持 private，避免外部跳过 execute() 直接置终态
    void markCompleted();
    void markFailed(const std::string& error_message = "Unknown error");

   private:
    std::string m_name;
    TaskFunc m_func;
    TaskType m_type = TaskType::Normal;
    // 2.4 / L-4: 状态与进度字段全部原子化，消除 setProgress/execute 间的数据竞争
    std::atomic<TaskStatus> m_status{TaskStatus::Pending};
    std::atomic<float> m_progress{0.0F};
    std::atomic<float> m_max_progress;

    std::atomic<bool> m_should_cancel{false};
    std::function<void()> m_completed_callback;

    // T-6：start_time 原子化（存 nanoseconds since epoch），消除潜在数据竞争
    // 使用 int64_t 而非 time_point，确保跨平台 trivially copyable
    std::atomic<int64_t> m_start_time_ns{0};

    // D-1：DI 注入的事件总线与结束回调
    IEventBus& m_event_bus;
    FinishedCallback m_on_finished;

    // M-6：删除 friend class TaskManager —— execute() 已为 public，
    //      通信通过 m_on_finished 回调，无需 Task 直接访问 TaskManager 私有成员

    // 输出缓冲（append_output/output，m_output_mutex 保护）
    mutable std::mutex m_output_mutex;
    std::string m_output;
};

// ============================================================
// ITaskManager 接口（D-1 DI 化）
// ============================================================

/// @brief 任务管理器抽象接口
/// @details 允许测试注入 MockTaskManager，解除对单例的硬依赖。
///          生产代码用 TaskManager（继承 ITaskManager）。
class WORKX_API ITaskManager {
   public:
    virtual ~ITaskManager() = default;

    virtual std::shared_ptr<Task> create(const std::string& name, Task::TaskFunc func,
                                         TaskType type = TaskType::Normal) = 0;

    virtual std::shared_ptr<Task> launch(const std::string& name, Task::TaskFunc func,
                                         TaskType type = TaskType::Normal) = 0;

    virtual void start(std::shared_ptr<Task> task) = 0;
    virtual void cancel(std::shared_ptr<Task> task) = 0;

    [[nodiscard]] virtual std::vector<std::shared_ptr<Task>> getTasks() const = 0;
    [[nodiscard]] virtual std::vector<std::shared_ptr<Task>> getRunningTasks() const = 0;
    [[nodiscard]] virtual size_t getRunningTaskCount() const = 0;

    /// @brief 按名称查找任务（#26：TaskStop/TaskOutput 定位任务用）
    /// @param name 任务名（AgentTool 生成的 task_id 即任务名）
    /// @return 找到返回任务指针；不存在返回 nullptr
    [[nodiscard]] virtual std::shared_ptr<Task> find_task(const std::string& name) const = 0;

    virtual void update() = 0;
    virtual void waitForAll() = 0;
    virtual void cancelAll() = 0;

    /// @brief 等待指定任务结束（H-9：替代 ChatSession 析构中的 sleep_for 轮询）
    /// @param task 待等待的任务
    /// @details 阻塞直到 task->isFinished() 为 true 或 30 秒兜底超时。
    ///          利用 Task::execute 结束时调用的 m_on_finished 回调通知 cv。
    virtual void wait(std::shared_ptr<Task> task) = 0;

    /// @brief 并行等待一批任务结束（M-2：替代逐个 wait 的最坏 N×30s）
    /// @param tasks 待等待的任务集合
    /// @details 单一 30 秒兜底超时，任一任务结束时唤醒重新检查，
    ///          全部 isFinished() 即返回。利用 m_on_finished 回调通知 cv。
    virtual void waitForTasks(const std::vector<std::shared_ptr<Task>>& tasks) = 0;
};

// ============================================================
// TaskManager 默认实现（基于 ThreadPool）
// ============================================================

class WORKX_API TaskManager final : public ITaskManager {
   public:
    /// @brief 单例访问（H-4：显式注入 EventBus::instance()，组装层使用）
    static TaskManager& instance() noexcept {
        static TaskManager inst(EventBus::instance());
        return inst;
    }

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    TaskManager(TaskManager&&) = delete;
    TaskManager& operator=(TaskManager&&) = delete;

    /// @brief 构造（H-4：DI 必须显式注入 IEventBus，无默认实参回退单例）
    explicit TaskManager(IEventBus& event_bus) : m_pool(0), m_event_bus(event_bus) {}

    std::shared_ptr<Task> create(const std::string& name, Task::TaskFunc func,
                                 TaskType type = TaskType::Normal) override;

    std::shared_ptr<Task> launch(const std::string& name, Task::TaskFunc func,
                                 TaskType type = TaskType::Normal) override;

    void start(std::shared_ptr<Task> task) override;
    void cancel(std::shared_ptr<Task> task) override;

    [[nodiscard]] std::vector<std::shared_ptr<Task>> getTasks() const override;
    [[nodiscard]] std::vector<std::shared_ptr<Task>> getRunningTasks() const override;
    [[nodiscard]] size_t getRunningTaskCount() const override;
    [[nodiscard]] std::shared_ptr<Task> find_task(const std::string& name) const override;

    void update() override;
    void waitForAll() override;
    void cancelAll() override;
    void wait(std::shared_ptr<Task> task) override;
    void waitForTasks(const std::vector<std::shared_ptr<Task>>& tasks) override;

    /// @brief 工作线程数（诊断 / 测试用）
    [[nodiscard]] size_t worker_count() const noexcept { return m_pool.worker_count(); }
    /// @brief 队列积压数（诊断用）
    [[nodiscard]] size_t pending_count() const { return m_pool.pending_count(); }

   private:
    ~TaskManager() override;

    std::vector<std::shared_ptr<Task>> m_entries;
    mutable std::mutex m_tasks_mutex;    // 保护 m_entries / 任务状态查询
    std::condition_variable m_tasks_cv;  // waitForAll 等待用
    std::mutex m_wait_mutex;  // H-D：wait(task) 独立锁，避免与 m_tasks_mutex 死锁
    std::condition_variable m_wait_cv;  // H-D：wait(task) 独立 cv，由 m_on_finished notify
    ThreadPool m_pool;
    IEventBus& m_event_bus;

    // M-6：删除 friend class Task —— Task 通过 m_on_finished 回调通知 cv，
    //      无需反向访问 TaskManager 私有成员，状态机可独立测试
};

}  // namespace agent
