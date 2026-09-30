/**
 * @file retry.h
 * @brief HTTP 重试策略（H-3）
 * @details 统一 Client 和 ChatSession 的重试逻辑：
 *          - 指数退避（exponential backoff）
 *          - 可配置最大重试次数、初始延迟、上限延迟
 *          - 区分可重试错误（429/500/502/503/504 + 网络错误）与不可重试错误
 *          - 总超时控制（与 H-2 StreamSession 总时长超时配合）
 *
 * 使用方式：
 * @code
 *   HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};
 *   if (policy.is_retryable(http_status, curl_error_msg)) {
 *       auto delay = policy.delay(attempt);
 *       sleep(delay);
 *       retry();
 *   }
 * @endcode
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "core/utils/error.h"  // V2-2：基于 Error::code 的重载

namespace agent {

/// @brief HTTP 重试策略
/// @details 不可变结构体，构造后即可被多线程共享读取
struct HttpRetryPolicy {
    int max_retries = 3;       ///< 最大重试次数（不含首次请求）
    int base_delay_ms = 1000;  ///< 初始退避延迟（毫秒）
    int max_delay_ms = 60000;  ///< 退避上限（毫秒，默认 60 秒）

    /// @brief 判断错误是否可重试
    /// @param http_status HTTP 状态码（0 表示无 HTTP 响应，如 curl 网络错误）
    /// @param error_msg 错误信息（curl_error 或自定义）
    /// @return true 表示可重试
    /// @details 可重试条件：
    ///          - HTTP 429（Too Many Requests）
    ///          - HTTP 500/502/503/504（服务器错误）
    ///          - http_status == 0 且 error_msg 非空（网络错误、连接超时）
    ///          不可重试条件：
    ///          - HTTP 4xx（除 429）：客户端错误，重试无益
    ///          - HTTP 200-299：成功
    ///          - 包含 "max iterations" 的错误：业务逻辑终止
    static bool is_retryable(unsigned int http_status, std::string_view error_msg) {
        // 业务逻辑错误：max iterations 不可重试
        if (error_msg.find("max iterations") != std::string_view::npos) {
            return false;
        }
        // HTTP 状态码判断
        if (http_status == 429) return true;                        // 限流
        if (http_status >= 500 && http_status <= 599) return true;  // 服务器错误
        if (http_status == 0 && !error_msg.empty()) return true;  // 网络错误（无 HTTP 响应）
        return false;
    }

    /// @brief V2-2：基于 Error::code 判断是否可重试
    /// @details 直接委托 Error::is_retryable()，与 Error::Code 体系对齐
    ///          可重试：NetworkTimeout/NetworkDisconnected/NetworkUnreachable/
    ///                 HttpRateLimited/HttpServerDown/StreamError
    static bool is_retryable(const Error& error) noexcept { return error.is_retryable(); }

    /// @brief 计算第 N 次重试的延迟（指数退避 + 上限）
    /// @param attempt 重试次数（0 = 首次请求后的第一次重试）
    /// @return 延迟时长
    /// @details 公式：min(base_delay_ms * 2^attempt, max_delay_ms)
    ///          C-3：保护 attempt >= 63 时的 UB（1LL << 63 在有符号 int64 下是 UB）
    ///          attempt 超过 62 直接返回 max_delay_ms（实际场景 max_retries 不会超过 10）
    std::chrono::milliseconds delay(int attempt) const {
        if (attempt >= 63) {
            return std::chrono::milliseconds(max_delay_ms);
        }
        int64_t d = static_cast<int64_t>(base_delay_ms) * (1LL << attempt);
        if (d > max_delay_ms) d = max_delay_ms;
        return std::chrono::milliseconds(d);
    }

    /// @brief 便捷方法：返回延迟的毫秒数
    /// @details C-3：不再标记 noexcept，因为 delay() 含分支判断逻辑复杂；
    ///               实际不会抛出，但移除 noexcept 避免契约误导
    int delay_ms(int attempt) const { return static_cast<int>(delay(attempt).count()); }
};

}  // namespace agent
