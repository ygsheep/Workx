/**
 * @file error.cpp
 * @brief 统一错误类型实现（V2）
 * @details Error 类型的非内联方法实现：code_string / to_string / 工厂函数
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/utils/error.h"

#include <format>

namespace agent {

std::string_view Error::code_string() const noexcept {
    switch (code) {
        case Code::Ok:
            return "Ok";
        case Code::NetworkTimeout:
            return "NetworkTimeout";
        case Code::NetworkDisconnected:
            return "NetworkDisconnected";
        case Code::NetworkUnreachable:
            return "NetworkUnreachable";
        case Code::HttpError:
            return "HttpError";
        case Code::HttpRateLimited:
            return "HttpRateLimited";
        case Code::HttpServerDown:
            return "HttpServerDown";
        case Code::InvalidInput:
            return "InvalidInput";
        case Code::MissingArgument:
            return "MissingArgument";
        case Code::InvalidFormat:
            return "InvalidFormat";
        case Code::PermissionDenied:
            return "PermissionDenied";
        case Code::ResourceNotFound:
            return "ResourceNotFound";
        case Code::AuthenticationFailed:
            return "AuthenticationFailed";
        case Code::Cancelled:
            return "Cancelled";
        case Code::InternalError:
            return "InternalError";
        case Code::NotImplemented:
            return "NotImplemented";
        case Code::ToolExecutionFailed:
            return "ToolExecutionFailed";
        case Code::ConfigInvalid:
            return "ConfigInvalid";
        case Code::ConfigMissing:
            return "ConfigMissing";
        case Code::ConfigParseFailed:
            return "ConfigParseFailed";
        case Code::StreamError:
            return "StreamError";
        case Code::StreamCancelled:
            return "StreamCancelled";
        case Code::Unknown:
            return "Unknown";
    }
    return "Unknown";
}

std::string Error::to_string() const {
    std::string result = std::format("[{}] {}", code_string(), message);
    if (!context.empty()) {
        result += std::format(" (context={})", context);
    }
    return result;
}

Error Error::from_http_response(unsigned int status_code, const std::string& body,
                                const std::string& curl_error) {
    // 网络错误（未到达服务器）
    if (status_code == 0 && !curl_error.empty()) {
        // 进一步根据 curl 错误内容推断（简单字符串匹配）
        if (curl_error.find("timed out") != std::string::npos ||
            curl_error.find("Timeout") != std::string::npos) {
            return {Code::NetworkTimeout, curl_error, {}};
        }
        if (curl_error.find("Couldn't resolve host") != std::string::npos ||
            curl_error.find("Could not resolve host") != std::string::npos) {
            return {Code::NetworkUnreachable, curl_error, {}};
        }
        if (curl_error.find("Connection refused") != std::string::npos ||
            curl_error.find("Connection reset") != std::string::npos ||
            curl_error.find("disconnected") != std::string::npos) {
            return {Code::NetworkDisconnected, curl_error, {}};
        }
        return {Code::NetworkDisconnected, curl_error, {}};
    }

    // HTTP 错误
    std::string ctx = std::format("status={}", status_code);
    if (status_code == 429) {
        return {Code::HttpRateLimited, "Rate limited by server", std::move(ctx)};
    }
    if (status_code >= 500 && status_code < 600) {
        return {Code::HttpServerDown, std::format("HTTP {} server error", status_code),
                std::move(ctx)};
    }
    if (status_code == 404) {
        return {Code::ResourceNotFound, std::format("HTTP 404 not found: {}", body),
                std::move(ctx)};
    }
    if (status_code == 401 || status_code == 403) {
        return {Code::AuthenticationFailed,
                std::format("HTTP {} authentication failed", status_code), std::move(ctx)};
    }
    if (status_code >= 400 && status_code < 600) {
        return {Code::HttpError, std::format("HTTP {} error: {}", status_code, body),
                std::move(ctx)};
    }

    // 不应该到达这里（2xx/3xx 不应构造错误）
    return {Code::Unknown, std::format("Unexpected status {}", status_code), std::move(ctx)};
}

Error Error::from_curl_code(int curl_code, const std::string& url) {
    // 常见 CURLcode 映射
    std::string msg;
    Code code = Code::NetworkDisconnected;

    switch (curl_code) {
        case 6:  // CURLE_COULDNT_RESOLVE_HOST
            code = Code::NetworkUnreachable;
            msg = "Could not resolve host";
            break;
        case 7:  // CURLE_COULDNT_CONNECT
            code = Code::NetworkDisconnected;
            msg = "Could not connect to server";
            break;
        case 28:  // CURLE_OPERATION_TIMEDOUT
            code = Code::NetworkTimeout;
            msg = "Operation timed out";
            break;
        case 35:  // CURLE_SSL_CONNECT_ERROR
            code = Code::NetworkDisconnected;
            msg = "SSL connect error";
            break;
        case 52:  // CURLE_GOT_NOTHING
            code = Code::NetworkDisconnected;
            msg = "Server returned nothing (no headers, no data)";
            break;
        case 56:  // CURLE_RECV_ERROR
            code = Code::NetworkDisconnected;
            msg = "Failure with receiving network data";
            break;
        default:
            code = Code::NetworkDisconnected;
            msg = std::format("CURL error code {}", curl_code);
            break;
    }

    return {code, std::move(msg), url};
}

Error Error::from_exception(const std::exception& e, std::string_view ctx) {
    std::string context{ctx};
    return {Code::InternalError, e.what(), std::move(context)};
}

bool operator==(const Error& lhs, const Error& rhs) noexcept { return lhs.code == rhs.code; }

bool operator!=(const Error& lhs, const Error& rhs) noexcept { return !(lhs == rhs); }

}  // namespace agent
