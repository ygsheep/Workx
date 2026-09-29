/**
 * @file test_balance_fetcher.cpp
 * @brief 余额拉取器单测（注入 fake HttpGet）
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/events/event_bus.h"
#include "island/balance_fetcher.h"
#include "island/events.h"

namespace {
bool close_enough(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

using agent::EventBus;
using agent::HttpResponse;
using island::BalanceFetcher;
using island::BalanceResult;
using island::BalanceUpdatedEvent;

namespace {

/// @brief fake GET：返回预设响应体 / 状态码 / 错误
class FakeGetter {
   public:
    std::atomic<int> calls{0};
    int status = 200;
    std::string body = R"({"is_available":true,"balance_infos":[
        {"currency":"CNY","total_balance":"12.34"}]})";
    std::string err_msg;

    BalanceFetcher::HttpGetFn fn() {
        return [this](const std::string& url,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      int timeout_ms) {
            ++calls;
            REQUIRE(url.find("/user/balance") != std::string::npos);
            REQUIRE(timeout_ms == 15000);
            bool has_auth = false;
            for (const auto& [k, v] : headers) {
                if (k == "Authorization" && v.rfind("Bearer ", 0) == 0) has_auth = true;
            }
            REQUIRE(has_auth);
            if (!err_msg.empty()) {
                return agent::ResultV2<HttpResponse>::err(agent::Error::Code::NetworkDisconnected,
                                                          err_msg, "fake_getter");
            }
            HttpResponse resp;
            resp.status_code = status;
            resp.body = body;
            return agent::ResultV2<HttpResponse>::ok(std::move(resp));
        };
    }
};

std::string cny_body(double cny) {
    return R"({"is_available":true,"balance_infos":[{"currency":"CNY","total_balance":")" +
           std::to_string(cny) + R"("}]})";
}

}  // namespace

TEST_CASE("balance: parse valid response computes usd from cny", "[island][balance]") {
    const BalanceResult r = BalanceFetcher::parse_balance_response(cny_body(72.0), 7.2);
    REQUIRE(r.success);
    REQUIRE(close_enough(r.cny_balance, 72.0));
    REQUIRE(close_enough(r.balance_usd, 10.0));
    REQUIRE_FALSE(r.error.empty() == false);  // success 时 error 为空
}

TEST_CASE("balance: parse rejects bad payloads", "[island][balance]") {
    REQUIRE_FALSE(BalanceFetcher::parse_balance_response("not json", 7.2).success);
    REQUIRE_FALSE(BalanceFetcher::parse_balance_response("[]", 7.2).success);
    REQUIRE_FALSE(BalanceFetcher::parse_balance_response(R"({"is_available":false})", 7.2).success);
    REQUIRE_FALSE(BalanceFetcher::parse_balance_response(R"({"is_available":true})", 7.2).success);
    REQUIRE_FALSE(
        BalanceFetcher::parse_balance_response(R"({"is_available":true,"balance_infos":[]})", 7.2)
            .success);
}

TEST_CASE("balance: fetch success publishes event and caches last", "[island][balance]") {
    FakeGetter fake;
    std::vector<BalanceResult> published;
    auto token = EventBus::instance().subscribe<BalanceUpdatedEvent>(
        [&](const BalanceUpdatedEvent& e) { published.push_back(e.result); });

    BalanceFetcher fetcher(EventBus::instance(), "sk-test", "https://api.deepseek.com", 7.2,
                           fake.fn(), std::chrono::seconds(1));
    fetcher.start();  // 启动即拉一次
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    fetcher.stop();
    EventBus::instance().process_async_events();  // BalanceUpdatedEvent 是 publish_async（仅入队）

    REQUIRE(fake.calls >= 1);
    REQUIRE(published.size() >= 1);
    REQUIRE(published.back().success);
    REQUIRE(published.back().source == "deepseek");
    REQUIRE(fetcher.last_result().success);
    EventBus::instance().unsubscribe<BalanceUpdatedEvent>(token);
}

TEST_CASE("balance: http error keeps last value and reports error", "[island][balance]") {
    FakeGetter fake;
    fake.status = 200;
    fake.body = cny_body(36.0);
    BalanceFetcher fetcher(EventBus::instance(), "sk", "https://api.deepseek.com", 7.2, fake.fn(),
                           std::chrono::seconds(3600));
    const auto ok = fetcher.refresh_and_wait(std::chrono::seconds(2));
    REQUIRE(ok.success);
    REQUIRE(close_enough(ok.balance_usd, 5.0));

    fake.body = "server error";
    fake.status = 500;
    const auto bad = fetcher.refresh_and_wait(std::chrono::seconds(2));
    REQUIRE_FALSE(bad.success);
    REQUIRE(bad.error.find("HTTP 500") != std::string::npos);
    // 保留上一次成功值
    REQUIRE(fetcher.last_result().success);
    REQUIRE(close_enough(fetcher.last_result().balance_usd, 5.0));
}

TEST_CASE("balance: 401 marks auth failure", "[island][balance]") {
    FakeGetter fake;
    fake.status = 401;
    fake.body = "{}";
    BalanceFetcher fetcher(EventBus::instance(), "sk-bad", "https://api.deepseek.com", 7.2,
                           fake.fn(), std::chrono::seconds(1));
    const auto r = fetcher.refresh_and_wait(std::chrono::seconds(2));
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.find("401") != std::string::npos);
}

TEST_CASE("balance: 401 freezes periodic fetch", "[island][balance][slow]") {
    FakeGetter fake;
    fake.status = 401;
    fake.body = "{}";
    BalanceFetcher fetcher(EventBus::instance(), "sk-bad", "https://api.deepseek.com", 7.2,
                           fake.fn(), std::chrono::seconds(1));
    fetcher.start();  // 启动即拉一次（401 → auth_failed）
    // 覆盖多个定时周期（1s）：401 冻结后定时拉取全部跳过，仅保留启动那 1 次
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    fetcher.stop();
    REQUIRE(fake.calls == 1);
}

TEST_CASE("balance: manual refresh releases 401 freeze", "[island][balance]") {
    FakeGetter fake;
    fake.status = 401;
    fake.body = "{}";
    BalanceFetcher fetcher(EventBus::instance(), "sk-bad", "https://api.deepseek.com", 7.2,
                           fake.fn(), std::chrono::hours(1));  // 定时周期极长，排除定时干扰
    const auto bad = fetcher.refresh_and_wait(std::chrono::seconds(2));  // 同步 401 → auth_failed
    REQUIRE_FALSE(bad.success);
    REQUIRE(bad.error.find("401") != std::string::npos);
    const int calls_after_401 = fake.calls.load();

    fake.status = 200;  // API Key 已修正
    fake.body = cny_body(12.34);
    const auto good = fetcher.refresh_and_wait(std::chrono::seconds(2));  // 手动刷新放行
    REQUIRE(good.success);                  // 手动刷新解除冻结
    REQUIRE(fake.calls > calls_after_401);  // 确实发起了新请求
    fetcher.stop();
}

TEST_CASE("balance: network error surfaces message", "[island][balance]") {
    FakeGetter fake;
    fake.err_msg = "connection refused";
    BalanceFetcher fetcher(EventBus::instance(), "sk", "https://api.deepseek.com", 7.2, fake.fn(),
                           std::chrono::seconds(1));
    const auto r = fetcher.refresh_and_wait(std::chrono::seconds(2));
    REQUIRE_FALSE(r.success);
    REQUIRE(r.error.find("connection refused") != std::string::npos);
}

TEST_CASE("balance: refresh_and_wait with 3s timeout returns cached on slow getter",
          "[island][balance][slow]") {
    FakeGetter fake_slow;
    std::atomic<int> slow_calls{0};
    BalanceFetcher::HttpGetFn slow_fn =
        [&](const std::string&, const std::vector<std::pair<std::string, std::string>>&, int) {
            ++slow_calls;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            HttpResponse resp;
            resp.status_code = 200;
            resp.body = cny_body(1.0);
            return agent::ResultV2<HttpResponse>::ok(std::move(resp));
        };

    BalanceFetcher fetcher(EventBus::instance(), "sk", "https://api.deepseek.com", 7.2, slow_fn,
                           std::chrono::hours(1));
    const auto r = fetcher.refresh_and_wait(std::chrono::milliseconds(700));
    REQUIRE_FALSE(r.success);  // 超时返回缓存/空值
    fetcher.stop();
    REQUIRE(slow_calls >= 1);
}
