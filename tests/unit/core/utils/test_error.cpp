/**
 * @file test_error.cpp
 * @brief Error 类型单元测试（V2）
 * @details 覆盖 Error 的构造、比较、便捷方法、工厂函数
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/utils/error.h"

#include <stdexcept>

using namespace agent;
using namespace Catch::Matchers;

TEST_CASE("Error default construction", "[error]") {
    Error e;
    REQUIRE(e.code == Error::Code::Unknown);
    REQUIRE(e.message.empty());
    REQUIRE(e.context.empty());
}

TEST_CASE("Error full construction", "[error]") {
    Error e{Error::Code::NetworkTimeout, "Request timed out", "url=https://api.example.com"};
    REQUIRE(e.code == Error::Code::NetworkTimeout);
    REQUIRE(e.message == "Request timed out");
    REQUIRE(e.context == "url=https://api.example.com");
}

TEST_CASE("Error code_string", "[error]") {
    REQUIRE(Error{Error::Code::Ok}.code_string() == "Ok");
    REQUIRE(Error{Error::Code::NetworkTimeout}.code_string() == "NetworkTimeout");
    REQUIRE(Error{Error::Code::NetworkDisconnected}.code_string() == "NetworkDisconnected");
    REQUIRE(Error{Error::Code::NetworkUnreachable}.code_string() == "NetworkUnreachable");
    REQUIRE(Error{Error::Code::HttpError}.code_string() == "HttpError");
    REQUIRE(Error{Error::Code::HttpRateLimited}.code_string() == "HttpRateLimited");
    REQUIRE(Error{Error::Code::HttpServerDown}.code_string() == "HttpServerDown");
    REQUIRE(Error{Error::Code::InvalidInput}.code_string() == "InvalidInput");
    REQUIRE(Error{Error::Code::MissingArgument}.code_string() == "MissingArgument");
    REQUIRE(Error{Error::Code::InvalidFormat}.code_string() == "InvalidFormat");
    REQUIRE(Error{Error::Code::PermissionDenied}.code_string() == "PermissionDenied");
    REQUIRE(Error{Error::Code::ResourceNotFound}.code_string() == "ResourceNotFound");
    REQUIRE(Error{Error::Code::AuthenticationFailed}.code_string() == "AuthenticationFailed");
    REQUIRE(Error{Error::Code::Cancelled}.code_string() == "Cancelled");
    REQUIRE(Error{Error::Code::InternalError}.code_string() == "InternalError");
    REQUIRE(Error{Error::Code::NotImplemented}.code_string() == "NotImplemented");
    REQUIRE(Error{Error::Code::ToolExecutionFailed}.code_string() == "ToolExecutionFailed");
    REQUIRE(Error{Error::Code::ConfigInvalid}.code_string() == "ConfigInvalid");
    REQUIRE(Error{Error::Code::ConfigMissing}.code_string() == "ConfigMissing");
    REQUIRE(Error{Error::Code::ConfigParseFailed}.code_string() == "ConfigParseFailed");
    REQUIRE(Error{Error::Code::StreamError}.code_string() == "StreamError");
    REQUIRE(Error{Error::Code::StreamCancelled}.code_string() == "StreamCancelled");
    REQUIRE(Error{Error::Code::Unknown}.code_string() == "Unknown");
}

TEST_CASE("Error to_string with context", "[error]") {
    Error e{Error::Code::NetworkTimeout, "Request timed out after 30000ms",
            "url=https://api.example.com"};
    REQUIRE_THAT(e.to_string(),
                 ContainsSubstring("[NetworkTimeout]") && ContainsSubstring("Request timed out"));
    REQUIRE_THAT(e.to_string(), ContainsSubstring("context=url=https://api.example.com"));
}

TEST_CASE("Error to_string without context", "[error]") {
    Error e{Error::Code::Unknown, "Something went wrong"};
    REQUIRE_THAT(e.to_string(),
                 ContainsSubstring("[Unknown]") && ContainsSubstring("Something went wrong"));
    REQUIRE_FALSE(e.to_string().find("context=") != std::string::npos);
}

TEST_CASE("Error is_retryable", "[error]") {
    // 可重试
    REQUIRE(Error{Error::Code::NetworkTimeout}.is_retryable());
    REQUIRE(Error{Error::Code::NetworkDisconnected}.is_retryable());
    REQUIRE(Error{Error::Code::NetworkUnreachable}.is_retryable());
    REQUIRE(Error{Error::Code::HttpRateLimited}.is_retryable());
    REQUIRE(Error{Error::Code::HttpServerDown}.is_retryable());
    REQUIRE(Error{Error::Code::StreamError}.is_retryable());

    // 不可重试
    REQUIRE_FALSE(Error{Error::Code::HttpError}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::InvalidInput}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::MissingArgument}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::InvalidFormat}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::PermissionDenied}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::ResourceNotFound}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::AuthenticationFailed}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::Cancelled}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::InternalError}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::NotImplemented}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::ToolExecutionFailed}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::ConfigInvalid}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::ConfigMissing}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::ConfigParseFailed}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::StreamCancelled}.is_retryable());
    REQUIRE_FALSE(Error{Error::Code::Unknown}.is_retryable());
}

TEST_CASE("Error is_network_error", "[error]") {
    REQUIRE(Error{Error::Code::NetworkTimeout}.is_network_error());
    REQUIRE(Error{Error::Code::NetworkDisconnected}.is_network_error());
    REQUIRE(Error{Error::Code::NetworkUnreachable}.is_network_error());
    REQUIRE_FALSE(Error{Error::Code::HttpError}.is_network_error());
    REQUIRE_FALSE(Error{Error::Code::InvalidInput}.is_network_error());
}

TEST_CASE("Error is_http_error", "[error]") {
    REQUIRE(Error{Error::Code::HttpError}.is_http_error());
    REQUIRE(Error{Error::Code::HttpRateLimited}.is_http_error());
    REQUIRE(Error{Error::Code::HttpServerDown}.is_http_error());
    REQUIRE_FALSE(Error{Error::Code::NetworkTimeout}.is_http_error());
    REQUIRE_FALSE(Error{Error::Code::InvalidInput}.is_http_error());
}

TEST_CASE("Error is_client_error", "[error]") {
    REQUIRE(Error{Error::Code::PermissionDenied}.is_client_error());
    REQUIRE(Error{Error::Code::ResourceNotFound}.is_client_error());
    REQUIRE(Error{Error::Code::AuthenticationFailed}.is_client_error());
    REQUIRE_FALSE(Error{Error::Code::HttpError}.is_client_error());
    REQUIRE_FALSE(Error{Error::Code::NetworkTimeout}.is_client_error());
}

TEST_CASE("Error is_config_error", "[error]") {
    REQUIRE(Error{Error::Code::ConfigInvalid}.is_config_error());
    REQUIRE(Error{Error::Code::ConfigMissing}.is_config_error());
    REQUIRE(Error{Error::Code::ConfigParseFailed}.is_config_error());
    REQUIRE_FALSE(Error{Error::Code::InvalidInput}.is_config_error());
}

TEST_CASE("Error is_stream_error", "[error]") {
    REQUIRE(Error{Error::Code::StreamError}.is_stream_error());
    REQUIRE(Error{Error::Code::StreamCancelled}.is_stream_error());
    REQUIRE_FALSE(Error{Error::Code::NetworkTimeout}.is_stream_error());
}

TEST_CASE("Error comparison operators", "[error]") {
    Error a{Error::Code::NetworkTimeout, "msg a"};
    Error b{Error::Code::NetworkTimeout, "msg b"};  // 相同 code，不同 message
    Error c{Error::Code::HttpError, "msg a"};       // 不同 code

    REQUIRE(a == b);  // 仅比较 code
    REQUIRE(a != c);
    REQUIRE_FALSE(a == c);
    REQUIRE_FALSE(a != b);
}

TEST_CASE("Error from_http_response network errors", "[error]") {
    SECTION("Timeout") {
        auto e = Error::from_http_response(0, "", "Operation timed out after 30000ms");
        REQUIRE(e.code == Error::Code::NetworkTimeout);
        REQUIRE_THAT(e.message, ContainsSubstring("timed out"));
    }

    SECTION("DNS failure") {
        auto e = Error::from_http_response(0, "", "Couldn't resolve host api.example.com");
        REQUIRE(e.code == Error::Code::NetworkUnreachable);
    }

    SECTION("Connection refused") {
        auto e = Error::from_http_response(0, "", "Connection refused");
        REQUIRE(e.code == Error::Code::NetworkDisconnected);
    }

    SECTION("Generic network error") {
        auto e = Error::from_http_response(0, "", "Some unknown curl error");
        REQUIRE(e.code == Error::Code::NetworkDisconnected);
    }
}

TEST_CASE("Error from_http_response http errors", "[error]") {
    SECTION("429 rate limited") {
        auto e = Error::from_http_response(429, "Too Many Requests", "");
        REQUIRE(e.code == Error::Code::HttpRateLimited);
        REQUIRE_THAT(e.context, ContainsSubstring("status=429"));
    }

    SECTION("500 server error") {
        auto e = Error::from_http_response(500, "Internal Server Error", "");
        REQUIRE(e.code == Error::Code::HttpServerDown);
        REQUIRE_THAT(e.context, ContainsSubstring("status=500"));
    }

    SECTION("503 service unavailable") {
        auto e = Error::from_http_response(503, "Service Unavailable", "");
        REQUIRE(e.code == Error::Code::HttpServerDown);
    }

    SECTION("404 not found") {
        auto e = Error::from_http_response(404, "Not Found", "");
        REQUIRE(e.code == Error::Code::ResourceNotFound);
        REQUIRE_THAT(e.message, ContainsSubstring("404"));
    }

    SECTION("401 unauthorized") {
        auto e = Error::from_http_response(401, "Unauthorized", "");
        REQUIRE(e.code == Error::Code::AuthenticationFailed);
    }

    SECTION("403 forbidden") {
        auto e = Error::from_http_response(403, "Forbidden", "");
        REQUIRE(e.code == Error::Code::AuthenticationFailed);
    }

    SECTION("400 bad request") {
        auto e = Error::from_http_response(400, "Bad Request", "");
        REQUIRE(e.code == Error::Code::HttpError);
        REQUIRE_THAT(e.context, ContainsSubstring("status=400"));
    }

    SECTION("409 conflict") {
        auto e = Error::from_http_response(409, "Conflict", "");
        REQUIRE(e.code == Error::Code::HttpError);
    }
}

TEST_CASE("Error from_curl_code", "[error]") {
    SECTION("CURLE_OPERATION_TIMEDOUT (28)") {
        auto e = Error::from_curl_code(28, "https://api.example.com");
        REQUIRE(e.code == Error::Code::NetworkTimeout);
        REQUIRE(e.context == "https://api.example.com");
        REQUIRE_THAT(e.message, ContainsSubstring("timed out"));
    }

    SECTION("CURLE_COULDNT_RESOLVE_HOST (6)") {
        auto e = Error::from_curl_code(6, "https://api.example.com");
        REQUIRE(e.code == Error::Code::NetworkUnreachable);
        REQUIRE(e.context == "https://api.example.com");
    }

    SECTION("CURLE_COULDNT_CONNECT (7)") {
        auto e = Error::from_curl_code(7, "https://api.example.com");
        REQUIRE(e.code == Error::Code::NetworkDisconnected);
    }

    SECTION("CURLE_SSL_CONNECT_ERROR (35)") {
        auto e = Error::from_curl_code(35, "https://api.example.com");
        REQUIRE(e.code == Error::Code::NetworkDisconnected);
        REQUIRE_THAT(e.message, ContainsSubstring("SSL"));
    }

    SECTION("CURLE_RECV_ERROR (56)") {
        auto e = Error::from_curl_code(56, "https://api.example.com");
        REQUIRE(e.code == Error::Code::NetworkDisconnected);
    }

    SECTION("Unknown curl code") {
        auto e = Error::from_curl_code(99, "https://api.example.com");
        REQUIRE(e.code == Error::Code::NetworkDisconnected);
        REQUIRE_THAT(e.message, ContainsSubstring("99"));
    }
}

TEST_CASE("Error from_exception", "[error]") {
    SECTION("runtime_error") {
        std::runtime_error ex("something failed");
        auto e = Error::from_exception(ex, "ChatSession::run_completion");
        REQUIRE(e.code == Error::Code::InternalError);
        REQUIRE(e.message == "something failed");
        REQUIRE(e.context == "ChatSession::run_completion");
    }

    SECTION("without context") {
        std::logic_error ex("invalid argument");
        auto e = Error::from_exception(ex);
        REQUIRE(e.code == Error::Code::InternalError);
        REQUIRE(e.message == "invalid argument");
        REQUIRE(e.context.empty());
    }
}
