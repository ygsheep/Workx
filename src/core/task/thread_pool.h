/**
 * @file thread_pool.h
 * @brief 固定大小线程池
 * @details 替代 TaskManager 中裸 std::thread，限制并发线程数，避免高并发时线程爆炸。
 *          - 任务队列 + 工作线程消费
 *          - enqueue 返回 std::future 以支持结果获取与异常传播
 *          - shutdown 优雅停止（等待队列排空后 join）
 *          - active_count 反映当前正在执行的任务数
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <future>
#include <memory>

#include "liblogger/logger.h"

namespace agent {

class ThreadPool {
   public:
    /// @brief 构造线程池并启动工作线程
    /// @param num_threads 工作线程数；0 视为 hardware_concurrency()，仍为 0 则退化为 1
    explicit ThreadPool(size_t num_threads = 0) {
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 1;
        }
        m_workers.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            m_workers.emplace_back([this]() { worker_loop(); });
        }
        LOG_INFO("[thread_pool] started, workers={}", num_threads);
    }

    ~ThreadPool() { shutdown(); }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    /// @brief 投递无返回值任务
    void enqueue(std::function<void()> task) {
        if (!task) return;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop) {
                // G-1：shutdown 后任务被丢弃，记录 WARN 便于排查丢失任务
                LOG_WARN("[thread_pool] enqueue after shutdown, task dropped");
                return;
            }
            m_tasks.push(std::move(task));
        }
        m_cv.notify_one();
    }

    /// @brief 投递带返回值任务，返回 future 供调用方等待结果
    template <typename F, typename... Args>
    auto enqueue_with_result(F&& f,
                             Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using ResultType = std::invoke_result_t<F, Args...>;
        auto task_ptr = std::make_shared<std::packaged_task<ResultType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<ResultType> fut = task_ptr->get_future();
        enqueue([task_ptr]() { (*task_ptr)(); });
        return fut;
    }

    /// @brief 优雅关闭：标记停止，唤醒所有工作线程，join 全部
    /// @details worker 在 m_stop=true 后会排空队列再退出（drain 语义），
    ///          因此正常 shutdown 后 m_tasks 应为空。若仍有残留，说明
    ///          worker 被异常中断 — 记录 WARN 便于排查。
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_stop) return;
            m_stop = true;
        }
        m_cv.notify_all();
        for (auto& w : m_workers) {
            if (w.joinable()) w.join();
        }
        // 所有 worker 已 join，无需持锁读取
        const size_t leftover = m_tasks.size();
        m_workers.clear();
        if (leftover > 0) {
            LOG_WARN("[thread_pool] shutdown complete, leftover_tasks={}", leftover);
        } else {
            LOG_INFO("[thread_pool] shutdown complete, all tasks drained");
        }
    }

    /// @brief 当前待执行任务数（队列积压）
    [[nodiscard]] size_t pending_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_tasks.size();
    }

    /// @brief 当前正在执行的任务数（活跃工作线程数）
    [[nodiscard]] size_t active_count() const { return m_active.load(std::memory_order_relaxed); }

    /// @brief 工作线程总数
    [[nodiscard]] size_t worker_count() const noexcept { return m_workers.size(); }

   private:
    void worker_loop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait(lock, [this]() { return m_stop || !m_tasks.empty(); });
                if (m_stop && m_tasks.empty()) return;
                task = std::move(m_tasks.front());
                m_tasks.pop();
            }
            m_active.fetch_add(1, std::memory_order_relaxed);
            LOG_DEBUG("[thread_pool] task dequeue, active={}, pending={}",
                      m_active.load(std::memory_order_relaxed), pending_count());
            try {
                task();
            } catch (const std::exception& e) {
                // G-1：吞掉任务异常，防止工作线程退出；记录 ERROR 便于排查
                LOG_ERROR("[thread_pool] task exception: {}", e.what());
            } catch (...) {
                LOG_ERROR("[thread_pool] task unknown exception");
            }
            m_active.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_active{0};
};

}  // namespace agent
