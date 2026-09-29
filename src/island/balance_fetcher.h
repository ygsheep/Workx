/**
 * @file balance_fetcher.h
 * @brief 余额拉取器（DeepSeek /user/balance）
 * @details 低频拉取：启动 1 次（start 后 trigger_refresh）+ 每 10min 定时
 *          + 任务完成（CostAccumulator 回调 trigger_refresh）+ GUI 手动请求
 *          （refresh_and_wait 同步拉取）。
 *          约束：HTTP 总超时 < 120s（项目硬约束），余额拉取用 15s 超时；
 *          复用 agent 层 HttpClient；API Key 无效（401）时停止定时拉取。
 *          失败保留上次值，下次周期重试（设计文档 8.1 错误矩阵）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/api/remote/http_client.h"
#include "core/events/i_event_bus.h"
#include "island/events.h"

namespace island {

/// @brief 余额拉取器
class BalanceFetcher {
   public:
    /// @brief GET 请求函数（默认走 HttpClient；测试注入 fake）
    using HttpGetFn = std::function<agent::ResultV2<agent::HttpResponse>(
        const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
        int timeout_ms)>;

    /// @param bus 事件总线（成功拉取后发布 BalanceUpdatedEvent）
    /// @param api_key DeepSeek API Key（Bearer 认证）
    /// @param base_url API 基础地址（如 https://api.deepseek.com）
    /// @param usd_cny_rate CNY → USD 折算汇率（配置 island.usd_cny_rate）
    /// @param getter HTTP GET 实现（默认 HttpClient）
    /// @param interval 定时拉取间隔（默认 10min）
    BalanceFetcher(agent::IEventBus& bus, std::string api_key, std::string base_url,
                   double usd_cny_rate, HttpGetFn getter = default_http_getter(),
                   std::chrono::seconds interval = std::chrono::minutes(10));

    BalanceFetcher(const BalanceFetcher&) = delete;
    BalanceFetcher& operator=(const BalanceFetcher&) = delete;
    ~BalanceFetcher();

    /// @brief 启动后台定时线程
    void start();

    /// @brief 停止后台线程（幂等）
    void stop();

    /// @brief 异步触发一次拉取（立即唤醒定时线程）
    void trigger_refresh();

    /// @brief 同步拉取一次并返回结果（GUI refresh_balance 请求，HTTP ≤3s 尽力）
    /// @param timeout 最长等待（超出返回最近一次结果）
    BalanceResult refresh_and_wait(std::chrono::milliseconds timeout);

    /// @brief 最近一次成功结果
    [[nodiscard]] BalanceResult last_result() const;

    /// @brief 解析 /user/balance 响应体（纯函数，供单测）
    /// @param body 响应 JSON：{"is_available":bool, "balance_infos":[{currency,total_balance}]}
    /// @param usd_cny_rate 汇率
    static BalanceResult parse_balance_response(const std::string& body, double usd_cny_rate);

    /// @brief 默认 HttpClient 实现（构造默认参数用）
    static HttpGetFn default_http_getter();

   private:
    void run_loop();
    BalanceResult do_fetch();

    agent::IEventBus& m_bus;
    std::string m_api_key;
    std::string m_base_url;
    double m_usd_cny_rate = 7.2;
    HttpGetFn m_getter;
    std::chrono::seconds m_interval;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_refresh_requested{false};
    std::atomic<bool> m_auth_failed{false};  ///< 401 后停止定时拉取（错误矩阵：高）
    mutable std::mutex m_mtx;                // last_result() 为 const，锁需 mutable
    std::condition_variable m_cv;
    BalanceResult m_last_result;
    std::shared_ptr<std::promise<BalanceResult>> m_pending_sync;  ///< refresh_and_wait 等待槽
    std::thread m_thread;
};

}  // namespace island