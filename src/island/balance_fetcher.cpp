/**
 * @file balance_fetcher.cpp
 * @brief 余额拉取器实现
 * @version 1.0.1
 * @date 2026-08
 */

#include "island/balance_fetcher.h"

#include <ctime>
#include <future>
#include <optional>

#include <nlohmann/json.hpp>

#include "island/events.h"

namespace island {

namespace {

constexpr int kHttpTimeoutMs = 15000;  // 设计文档：余额拉取激进 15s 超时（硬约束 <120s）

/// @brief 从 balance_infos 中提取 CNY 余额（缺失 CNY 时取第一条）
std::optional<double> extract_cny(const nlohmann::json& balance_infos) {
    if (!balance_infos.is_array() || balance_infos.empty()) return std::nullopt;
    for (const auto& info : balance_infos) {
        if (info.value("currency", "") == "CNY") {
            try {
                return std::stod(info.value("total_balance", "0"));
            } catch (const std::exception&) {
                return std::nullopt;
            }
        }
    }
    const auto& first = balance_infos.front();
    try {
        return std::stod(first.value("total_balance", "0"));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

}  // namespace

BalanceFetcher::BalanceFetcher(agent::IEventBus& bus, std::string api_key, std::string base_url,
                               double usd_cny_rate, HttpGetFn getter, std::chrono::seconds interval)
    : m_bus(bus),
      m_api_key(std::move(api_key)),
      m_base_url(std::move(base_url)),
      m_usd_cny_rate(usd_cny_rate > 0.0 ? usd_cny_rate : 7.2),
      m_getter(std::move(getter)),
      m_interval(interval) {}

BalanceFetcher::~BalanceFetcher() { stop(); }

void BalanceFetcher::start() {
    if (m_thread.joinable()) return;
    m_stop.store(false);
    m_thread = std::thread([this] { run_loop(); });
}

void BalanceFetcher::stop() {
    if (!m_thread.joinable()) return;
    m_stop.store(true);
    m_cv.notify_all();
    m_thread.join();
}

void BalanceFetcher::trigger_refresh() {
    m_refresh_requested.store(true);
    m_cv.notify_all();
}

BalanceResult BalanceFetcher::refresh_and_wait(std::chrono::milliseconds timeout) {
    // GUI 手动刷新：复用后台线程拉取，等待至多 timeout；超时返回最近一次结果（3s 兜底）
    if (!m_thread.joinable()) start();  // 未启动时按需启动后台循环
    auto promise = std::make_shared<std::promise<BalanceResult>>();
    {
        std::lock_guard<std::mutex> lock(m_mtx);
        m_pending_sync = promise;
    }
    trigger_refresh();
    auto future = promise->get_future();
    if (future.wait_for(timeout) == std::future_status::ready) {
        return future.get();
    }
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_last_result;
}

BalanceResult BalanceFetcher::last_result() const {
    std::lock_guard<std::mutex> lock(m_mtx);
    return m_last_result;
}

BalanceResult BalanceFetcher::do_fetch() {
    BalanceResult r;
    r.fetched_at = static_cast<int64_t>(std::time(nullptr));

    const std::string url = m_base_url + "/user/balance";
    const std::vector<std::pair<std::string, std::string>> headers{
        {"Authorization", "Bearer " + m_api_key},
    };
    auto resp = m_getter(url, headers, kHttpTimeoutMs);
    if (resp.is_err()) {
        r.error = "网络错误: " + resp.error().message;
        return r;
    }
    const auto& http = resp.value();
    if (!http.is_success()) {
        if (http.status_code == 401) {
            m_auth_failed.store(true);  // 停止后续定时拉取
            r.error = "API Key 无效（401）";
        } else if (http.status_code == 429) {
            r.error = "限流（429），稍后重试";
        } else {
            r.error = "HTTP " + std::to_string(http.status_code);
        }
        return r;
    }

    r = parse_balance_response(http.body, m_usd_cny_rate);
    r.fetched_at = static_cast<int64_t>(std::time(nullptr));
    if (r.success) {
        r.source = "deepseek";
    }
    return r;
}

BalanceResult BalanceFetcher::parse_balance_response(const std::string& body, double usd_cny_rate) {
    BalanceResult r;
    r.fetched_at = static_cast<int64_t>(std::time(nullptr));

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception& e) {
        r.error = std::string("响应解析失败: ") + e.what();
        return r;
    }
    if (!j.is_object()) {
        r.error = "响应格式错误（非对象）";
        return r;
    }
    if (!j.value("is_available", false)) {
        r.error = "账户不可用（is_available=false）";
        return r;
    }
    if (!j.contains("balance_infos")) {
        r.error = "响应缺少 balance_infos";
        return r;
    }
    const auto cny = extract_cny(j["balance_infos"]);
    if (!cny.has_value()) {
        r.error = "balance_infos 解析失败";
        return r;
    }
    r.cny_balance = *cny;
    r.balance_usd = (*cny) / (usd_cny_rate > 0.0 ? usd_cny_rate : 7.2);
    r.success = true;
    return r;
}

BalanceFetcher::HttpGetFn BalanceFetcher::default_http_getter() {
    return
        [](const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
           int timeout_ms) -> agent::ResultV2<agent::HttpResponse> {
            agent::HttpClient http;
            return http.get(url, headers, timeout_ms);
        };
}

void BalanceFetcher::run_loop() {
    // 启动即拉一次（设计文档 6.4：trigger #1）+ 触发刷新/定时循环
    trigger_refresh();
    while (!m_stop.load()) {
        std::unique_lock<std::mutex> lock(m_mtx);
        m_cv.wait_for(lock, m_interval,
                      [this] { return m_stop.load() || m_refresh_requested.load(); });
        const bool manual = m_refresh_requested.exchange(false);
        lock.unlock();

        if (m_stop.load()) break;
        // 401 后：定时拉取跳过，手动刷新放行（可解除冻结）
        if (m_auth_failed.load() && !manual) continue;

        const BalanceResult result = do_fetch();
        if (result.success) {
            if (m_auth_failed.load()) {
                m_auth_failed.store(false);  // 手动刷新可解除 401 冻结
            }
            {
                std::lock_guard<std::mutex> guard(m_mtx);
                m_last_result = result;
            }
            BalanceUpdatedEvent ev{result};
            m_bus.publish_async(ev);
        }
        // 同步等待槽：无论成败都交付（refresh_and_wait 兜底语义）
        {
            std::lock_guard<std::mutex> guard(m_mtx);
            if (m_pending_sync) {
                m_pending_sync->set_value(result);
                m_pending_sync.reset();
            }
        }
        // 失败：保留上次值，下次周期重试（错误矩阵：预期场景）
    }
}

}  // namespace island