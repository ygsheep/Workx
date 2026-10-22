/**
 * @file background_task_tracker.h
 * @brief BackgroundTaskTracker — 后台任务结果回传追踪器
 * @details shell 工具（Bash/PowerShell）以 run_in_background=true 启动的后台任务，
 *          此前是"发射后不管"：结果只存进进程内注册表并发布 UI 事件，从不回传给模型，
 *          模型只能靠后续轮询（且注册表无读取者）——等于结果丢失。
 *
 *          本追踪器补齐这条链路：
 *          - 工具启动后台任务前 add_pending()，任务结束（成功/失败/取消）时 mark_done()
 *            暂存格式化输出；
 *          - ReActLoop 持有一个实例并经 ToolContext 注入工具，在工具轮边界
 *            drain_completed() 取回结果注入对话；
 *          - 模型想收尾但仍有未完成任务时，用 has_pending() 判断并等待，
 *            保证"非阻塞"的同时结果必达。
 *
 * @note 线程安全：add_pending/mark_done 在 TaskManager 工作线程调用，
 *       drain_completed/has_pending 在 ReAct 主线程调用，内部以互斥锁保护。
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace agent::tool {

/// @brief 一条已完成的后台任务结果
struct BackgroundTaskResult {
    std::string task_name;  ///< TaskManager 任务名（如 bash:3:make -j8）
    std::string tool_name;  ///< 发起工具名（Bash / PowerShell）
    std::string output;  ///< 已格式化的输出（stdout/stderr/exit code，已截断 + 脱敏）
    bool is_error = false;  ///< 命令是否以失败收场（启动失败 / 非零退出 / 超时 / 取消）
};

/// @brief 后台任务结果追踪器（线程安全）
///
/// 生命周期由 ReActLoop 通过 shared_ptr 持有并注入 ToolContext；后台任务 lambda
/// 捕获该 shared_ptr 的副本，因此即使 ReActLoop 所在轮次结束后任务才完成，
/// 追踪器对象也不会提前析构（结果仍可安全写入）。
class BackgroundTaskTracker {
   public:
    /// @brief 创建共享实例
    static std::shared_ptr<BackgroundTaskTracker> create() {
        return std::make_shared<BackgroundTaskTracker>();
    }

    BackgroundTaskTracker() = default;
    BackgroundTaskTracker(const BackgroundTaskTracker&) = delete;
    BackgroundTaskTracker& operator=(const BackgroundTaskTracker&) = delete;

    /// @brief 登记一个即将启动的后台任务
    /// @note 必须在 TaskManager::launch **之前** 调用：若任务瞬间完成，
    ///       mark_done 会先于 add_pending 到达，导致该任务永久停留在 pending。
    void add_pending(const std::string& task_name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pending.push_back(task_name);
    }

    /// @brief 撤销登记（任务未成功启动时调用，避免污染 pending 集合）
    void remove_pending(const std::string& task_name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (*it == task_name) {
                m_pending.erase(it);
                break;
            }
        }
    }

    /// @brief 标记任务完成：移出 pending，暂存结果供 drain 取回
    void mark_done(BackgroundTaskResult result) {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
            if (*it == result.task_name) {
                m_pending.erase(it);
                break;
            }
        }
        m_completed.push_back(std::move(result));
    }

    /// @brief 是否仍有未完成的后台任务
    bool has_pending() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return !m_pending.empty();
    }

    /// @brief 未完成任务数
    size_t pending_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_pending.size();
    }

    /// @brief 取走全部已完成结果（清空已完成队列，不影响 pending）
    std::vector<BackgroundTaskResult> drain_completed() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<BackgroundTaskResult> out;
        out.swap(m_completed);
        return out;
    }

   private:
    mutable std::mutex m_mutex;
    std::vector<std::string> m_pending;             ///< 未完成任务名（向量，量小）
    std::vector<BackgroundTaskResult> m_completed;  ///< 已完成待取回的结果
};

}  // namespace agent::tool
