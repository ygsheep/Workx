/**
 * @file test_http_response.cpp
 * @brief HttpResponse 便捷方法单元测试（E-6）
 * @details 验证 HTTP 响应状态判断：
 *          - is_success: 2xx + 空 error
 *          - is_http_error: 4xx/5xx + 空 error
 *          - is_network_error: status=0 + 非空 error
 *          - is_client_error / is_server_error / is_rate_limited
 *          - is_retryable: 429 + 5xx + 网络错误
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/api/remote/http_client.h"

using namespace agent;

// ============================================================
// is_success (2xx)
// ============================================================

TEST_CASE("HttpResponse is_success 2xx", "[http_response][is_success]") {
    HttpResponse r{.status_code = 200, .body = "OK", .error = ""};
    REQUIRE(r.is_success());

    HttpResponse r2{.status_code = 201, .body = "Created", .error = ""};
    REQUIRE(r2.is_success());

    HttpResponse r3{.status_code = 204, .body = "", .error = ""};
    REQUIRE(r3.is_success());
}

TEST_CASE("HttpResponse is_success 3xx not success", "[http_response][is_success]") {
    // 3xx 重定向不算 success（CURLOPT_FOLLOWLOCATION=0 时不自动跟随）
    HttpResponse r{.status_code = 301, .body = "", .error = ""};
    REQUIRE_FALSE(r.is_success());
}

TEST_CASE("HttpResponse is_success 4xx not success", "[http_response][is_success]") {
    HttpResponse r{.status_code = 404, .body = "Not Found", .error = ""};
    REQUIRE_FALSE(r.is_success());
}

TEST_CASE("HttpResponse is_success network error not success", "[http_response][is_success]") {
    HttpResponse r{.status_code = 0, .body = "", .error = "Connection timed out"};
    REQUIRE_FALSE(r.is_success());
}

// ============================================================
// is_http_error (4xx/5xx)
// ============================================================

TEST_CASE("HttpResponse is_http_error 4xx", "[http_response][is_http_error]") {
    HttpResponse r{.status_code = 400, .body = "Bad Request", .error = ""};
    REQUIRE(r.is_http_error());

    HttpResponse r2{.status_code = 401, .body = "Unauthorized", .error = ""};
    REQUIRE(r2.is_http_error());

    HttpResponse r3{.status_code = 404, .body = "Not Found", .error = ""};
    REQUIRE(r3.is_http_error());
}

TEST_CASE("HttpResponse is_http_error 5xx", "[http_response][is_http_error]") {
    HttpResponse r{.status_code = 500, .body = "Internal Server Error", .error = ""};
    REQUIRE(r.is_http_error());

    HttpResponse r2{.status_code = 503, .body = "Service Unavailable", .error = ""};
    REQUIRE(r2.is_http_error());
}

TEST_CASE("HttpResponse is_http_error 2xx not error", "[http_response][is_http_error]") {
    HttpResponse r{.status_code = 200, .body = "OK", .error = ""};
    REQUIRE_FALSE(r.is_http_error());
}

TEST_CASE("HttpResponse is_http_error network error not http error",
          "[http_response][is_http_error]") {
    // 网络错误时 status_code=0 且 error 非空，不算 HTTP 错误
    HttpResponse r{.status_code = 0, .body = "", .error = "curl timeout"};
    REQUIRE_FALSE(r.is_http_error());
}

// ============================================================
// is_network_error (curl 失败)
// ============================================================

TEST_CASE("HttpResponse is_network_error", "[http_response][is_network_error]") {
    HttpResponse r{.status_code = 0, .body = "", .error = "Connection timed out"};
    REQUIRE(r.is_network_error());

    HttpResponse r2{.status_code = 0, .body = "", .error = "DNS resolution failed"};
    REQUIRE(r2.is_network_error());
}

TEST_CASE("HttpResponse is_network_error 2xx not network error",
          "[http_response][is_network_error]") {
    HttpResponse r{.status_code = 200, .body = "OK", .error = ""};
    REQUIRE_FALSE(r.is_network_error());
}

TEST_CASE("HttpResponse is_network_error 4xx not network error",
          "[http_response][is_network_error]") {
    HttpResponse r{.status_code = 404, .body = "Not Found", .error = ""};
    REQUIRE_FALSE(r.is_network_error());
}

// ============================================================
// is_client_error / is_server_error / is_rate_limited
// ============================================================

TEST_CASE("HttpResponse is_client_error 4xx", "[http_response][is_client_error]") {
    HttpResponse r{.status_code = 400, .body = "", .error = ""};
    REQUIRE(r.is_client_error());

    HttpResponse r2{.status_code = 404, .body = "", .error = ""};
    REQUIRE(r2.is_client_error());

    HttpResponse r3{.status_code = 499, .body = "", .error = ""};
    REQUIRE(r3.is_client_error());
}

TEST_CASE("HttpResponse is_client_error 5xx not client", "[http_response][is_client_error]") {
    HttpResponse r{.status_code = 500, .body = "", .error = ""};
    REQUIRE_FALSE(r.is_client_error());
}

TEST_CASE("HttpResponse is_server_error 5xx", "[http_response][is_server_error]") {
    HttpResponse r{.status_code = 500, .body = "", .error = ""};
    REQUIRE(r.is_server_error());

    HttpResponse r2{.status_code = 502, .body = "", .error = ""};
    REQUIRE(r2.is_server_error());

    HttpResponse r3{.status_code = 599, .body = "", .error = ""};
    REQUIRE(r3.is_server_error());
}

TEST_CASE("HttpResponse is_server_error 4xx not server", "[http_response][is_server_error]") {
    HttpResponse r{.status_code = 404, .body = "", .error = ""};
    REQUIRE_FALSE(r.is_server_error());
}

TEST_CASE("HttpResponse is_rate_limited 429", "[http_response][is_rate_limited]") {
    HttpResponse r{.status_code = 429, .body = "Too Many Requests", .error = ""};
    REQUIRE(r.is_rate_limited());
}

TEST_CASE("HttpResponse is_rate_limited 500 not rate limited", "[http_response][is_rate_limited]") {
    HttpResponse r{.status_code = 500, .body = "", .error = ""};
    REQUIRE_FALSE(r.is_rate_limited());
}

// ============================================================
// is_retryable (与 HttpRetryPolicy 配对)
// ============================================================

TEST_CASE("HttpResponse is_retryable 429", "[http_response][is_retryable]") {
    HttpResponse r{.status_code = 429, .body = "", .error = ""};
    REQUIRE(r.is_retryable());
}

TEST_CASE("HttpResponse is_retryable 5xx", "[http_response][is_retryable]") {
    HttpResponse r{.status_code = 500, .body = "", .error = ""};
    REQUIRE(r.is_retryable());

    HttpResponse r2{.status_code = 502, .body = "", .error = ""};
    REQUIRE(r2.is_retryable());

    HttpResponse r3{.status_code = 503, .body = "", .error = ""};
    REQUIRE(r3.is_retryable());

    HttpResponse r4{.status_code = 504, .body = "", .error = ""};
    REQUIRE(r4.is_retryable());
}

TEST_CASE("HttpResponse is_retryable network error", "[http_response][is_retryable]") {
    HttpResponse r{.status_code = 0, .body = "", .error = "Connection timed out"};
    REQUIRE(r.is_retryable());
}

TEST_CASE("HttpResponse is_retryable 4xx not retryable", "[http_response][is_retryable]") {
    // 4xx（除 429）不可重试
    HttpResponse r{.status_code = 400, .body = "", .error = ""};
    REQUIRE_FALSE(r.is_retryable());

    HttpResponse r2{.status_code = 401, .body = "", .error = ""};
    REQUIRE_FALSE(r2.is_retryable());

    HttpResponse r3{.status_code = 403, .body = "", .error = ""};
    REQUIRE_FALSE(r3.is_retryable());

    HttpResponse r4{.status_code = 404, .body = "", .error = ""};
    REQUIRE_FALSE(r4.is_retryable());
}

TEST_CASE("HttpResponse is_retryable 2xx not retryable", "[http_response][is_retryable]") {
    HttpResponse r{.status_code = 200, .body = "OK", .error = ""};
    REQUIRE_FALSE(r.is_retryable());
}
