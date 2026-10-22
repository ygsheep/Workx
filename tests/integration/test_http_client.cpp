/**
 * @file test_http_client.cpp
 * @brief HttpClient 单元测试（libcurl 版本）
 * @details 使用 LM Studio OpenAI 兼容 API 作为测试服务器
 *          需要先启动 LM Studio 服务器（默认 http://127.0.0.1:1234）
 *          可通过环境变量 LM_STUDIO_BASE_URL 自定义地址
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/api/remote/http_client.h"
#include "agent/api/remote/sse_stream_reader.h"
#include "test_server_fixture.h"

#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

using namespace agent;

// ============================================================
// 测试辅助：自动启停测试服务器
// ============================================================
// 默认行为：自动启动 tests/integration/fixtures/test_server.py（X-2 修复）
// 兼容：若设置环境变量 LM_STUDIO_BASE_URL，则使用 LM Studio
//
// 用法：
//   1. 直接运行 workx_integration_tests.exe（自动启 Python 服务器）
//   2. 设置 LM_STUDIO_BASE_URL=http://127.0.0.1:1234 后运行（用 LM Studio）

static std::string s_base_url;
static std::unique_ptr<test::AutoTestServer> s_test_server;

struct TestServerFixture {
    TestServerFixture() {
        static bool initialized = false;
        if (!initialized) {
            s_test_server = std::make_unique<test::AutoTestServer>();
            REQUIRE(s_test_server->is_ready());
            s_base_url = s_test_server->base_url();
            REQUIRE_FALSE(s_base_url.empty());

            // 连通性测试
            HttpClient check_client;
            auto result = check_client.get(s_base_url + "/v1/models", {}, 5000);
            bool ok = false;
            if (result.is_ok() && result.value().status_code == 200) {
                ok = true;
            }
            if (!ok) {
                FAIL("测试服务器未就绪: " + s_base_url);
            }
            initialized = true;
        }
    }
};

// ============================================================
// URL 解析测试（纯文本，无需服务器）
// ============================================================

TEST_CASE("HttpClient parse_url", "[http][client]") {
    SECTION("simple https url") {
        auto p = HttpClient::parse_url("https://api.openai.com/v1/models");
        REQUIRE(p.scheme == "https");
        REQUIRE(p.host == "api.openai.com");
        REQUIRE(p.port == "443");
        REQUIRE(p.target == "/v1/models");
    }

    SECTION("url with query string") {
        auto p = HttpClient::parse_url("https://example.com/path?key=val&page=1");
        REQUIRE(p.scheme == "https");
        REQUIRE(p.host == "example.com");
        REQUIRE(p.port == "443");
        REQUIRE(p.target == "/path?key=val&page=1");
    }

    SECTION("http url with custom port") {
        auto p = HttpClient::parse_url("http://localhost:8080/api/test");
        REQUIRE(p.scheme == "http");
        REQUIRE(p.host == "localhost");
        REQUIRE(p.port == "8080");
        REQUIRE(p.target == "/api/test");
    }

    SECTION("url without path") {
        auto p = HttpClient::parse_url("https://api.example.com");
        REQUIRE(p.scheme == "https");
        REQUIRE(p.host == "api.example.com");
        REQUIRE(p.port == "443");
        REQUIRE(p.target == "/");
    }

    SECTION("url with trailing slash") {
        auto p = HttpClient::parse_url("https://api.deepseek.com/");
        REQUIRE(p.scheme == "https");
        REQUIRE(p.host == "api.deepseek.com");
        REQUIRE(p.target == "/");
    }
}

// ============================================================
// 同步 GET 测试（需要 LM Studio）
// ============================================================

TEST_CASE_METHOD(TestServerFixture, "HttpClient sync GET", "[http][client]") {
    HttpClient client;

    SECTION("GET /v1/models returns 200") {
        auto result = client.get(s_base_url + "/v1/models", {}, 5000);
        REQUIRE(result.is_ok());
        REQUIRE(result.value().status_code == 200);
    }

    SECTION("GET /v1/models returns valid JSON") {
        auto result = client.get(s_base_url + "/v1/models", {}, 5000);
        REQUIRE(result.is_ok());
        REQUIRE(result.value().status_code == 200);
        REQUIRE(result.value().body.find("id") != std::string::npos);
        REQUIRE(result.value().body.find("object") != std::string::npos);
    }

    SECTION("GET with custom headers") {
        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer lm-studio"}, {"X-Custom", "value"}};
        auto result = client.get(s_base_url + "/v1/models", headers, 5000);
        REQUIRE(result.is_ok());
        REQUIRE(result.value().status_code == 200);
    }

    SECTION("GET invalid LM Studio API path returns error") {
        // LM Studio 只路由已知端点，未知路径可能返回 200（回退到 LLM）
        // 改为测试无效 URL 情况
        auto result = client.get("http://127.0.0.1:16543/v1/nonexistent", {}, 1000);
        REQUIRE(result.is_err());
    }

    SECTION("GET invalid URL returns error") {
        auto result = client.get("http://192.0.2.1:1/nonexistent", {}, 1000);
        REQUIRE(result.is_err());
    }

    SECTION("GET timeout returns error") {
        auto result = client.get("http://203.0.113.1:1/slow", {}, 1000);
        REQUIRE(result.is_err());
    }
}

// ============================================================
// 异步流式 POST 测试（需要 LM Studio 且已加载模型）
// ============================================================

TEST_CASE_METHOD(TestServerFixture, "HttpClient async POST stream", "[http][client]") {
    HttpClient client;

    SECTION("streaming chat completions returns SSE chunks") {
        std::mutex mtx;
        bool completed = false;
        std::condition_variable cv;

        auto parse_cb = [](const std::string&, const std::string& data, StreamChunk& out) -> bool {
            out.content_delta = data;
            return true;
        };
        auto reader = std::make_shared<SSEStreamReader>(std::move(parse_cb));

        std::vector<std::pair<std::string, std::string>> headers = {
            {"Content-Type", "application/json"}, {"Accept", "text/event-stream"}};

        client.async_post_stream(
            s_base_url + "/v1/chat/completions", headers,
            R"({"model":"local-model","messages":[{"role":"user","content":"hi"}],"stream":true})",
            reader,
            [&]() {
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    completed = true;
                }
                cv.notify_one();
            },
            15000);

        {
            std::unique_lock<std::mutex> lk(mtx);
            cv.wait_for(lk, std::chrono::seconds(20), [&]() { return completed; });
        }

        REQUIRE(completed);
    }
}

// ============================================================
// cancel 测试
// ============================================================

TEST_CASE_METHOD(TestServerFixture, "HttpClient cancel stream", "[http][client]") {
    HttpClient client;

    auto parse_cb = [](const std::string&, const std::string& data, StreamChunk& out) -> bool {
        out.content_delta = data;
        return true;
    };
    auto reader = std::make_shared<SSEStreamReader>(std::move(parse_cb));

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}, {"Accept", "text/event-stream"}};

    bool on_complete_called = false;
    std::mutex mtx;
    std::condition_variable cv;

    client.async_post_stream(
        s_base_url + "/v1/chat/completions", headers,
        R"({"model":"local-model","messages":[{"role":"user","content":"hi"}],"stream":true})",
        reader,
        [&]() {
            {
                std::lock_guard<std::mutex> lk(mtx);
                on_complete_called = true;
            }
            cv.notify_one();
        },
        15000);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    client.cancel_stream(reader.get());

    // cancel 后 reader 应很快标记为 finished
    for (int i = 0; i < 50; ++i) {
        if (reader->is_finished()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    REQUIRE(reader->is_finished());
}

// ============================================================
// shutdown 测试
// ============================================================

TEST_CASE_METHOD(TestServerFixture, "HttpClient shutdown", "[http][client]") {
    SECTION("shutdown without active requests") {
        HttpClient client;
        client.shutdown();
    }

    SECTION("shutdown during active request") {
        HttpClient client;

        auto parse_cb = [](const std::string&, const std::string& data, StreamChunk& out) -> bool {
            out.content_delta = data;
            return true;
        };
        auto reader = std::make_shared<SSEStreamReader>(std::move(parse_cb));

        std::vector<std::pair<std::string, std::string>> headers = {
            {"Content-Type", "application/json"}, {"Accept", "text/event-stream"}};

        client.async_post_stream(
            s_base_url + "/v1/chat/completions", headers,
            R"({"model":"local-model","messages":[{"role":"user","content":"hi"}],"stream":true})",
            reader, []() {}, 15000);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // shutdown 时应安全清理所有活跃 session，不会崩溃
        REQUIRE_NOTHROW(client.shutdown());
    }
}