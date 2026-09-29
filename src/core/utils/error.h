/**
 * @file error.h
 * @brief 统一错误类型（V2）
 * @details 携带错误码 + 消息 + 上下文，替代 Result<T, std::string> 中的字符串错误。
 *          按"错误来源"分类，不按 HTTP 状态码分类（避免业务层依赖 HTTP 细节）。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>

namespace agent {

/// @brief 统一错误类型（V2）
/// @details 携带错误码 + 消息 + 上下文，替代 Result<T, std::string> 中的字符串错误
struct Error {
    /// @brief 错误码枚举
    /// @details 按"错误来源"分类，不按 HTTP 状态码分类（避免业务层依赖 HTTP 细节）
    enum class Code : int {
        Ok = 0,  ///< 成功（不应出现在 Error 中，仅用于内部断言）

        // === 网络类（1xx）===
        NetworkTimeout = 100,       ///< 网络超时（连接/读取/总时长）
        NetworkDisconnected = 101,  ///< 连接断开（DNS 失败、TCP reset）
        NetworkUnreachable = 102,   ///< 网络不可达

        // === HTTP 类（2xx）===
        HttpError = 200,        ///< HTTP 4xx/5xx（具体状态码见 context）
        HttpRateLimited = 201,  ///< HTTP 429 限流
        HttpServerDown = 202,   ///< HTTP 5xx 服务端错误

        // === 输入类（3xx）===
        InvalidInput = 300,     ///< 输入参数无效（JSON 解析失败、类型不匹配）
        MissingArgument = 301,  ///< 缺少必填参数
        InvalidFormat = 302,    ///< 格式错误（如 URL 解析失败）

        // === 权限类（4xx）===
        PermissionDenied = 400,      ///< 权限拒绝（工具不允许执行）
        ResourceNotFound = 401,      ///< 资源不存在（文件/配置键/模型）
        AuthenticationFailed = 402,  ///< 认证失败（API key 无效）

        // === 业务类（5xx）===
        Cancelled = 500,            ///< 操作被取消（用户中断/超时取消）
        InternalError = 501,        ///< 内部错误（不变量违反、不应到达的状态）
        NotImplemented = 502,       ///< 功能未实现（TODO 工具）
        ToolExecutionFailed = 503,  ///< 工具执行失败（工具内部异常）

        // === 配置类（6xx）===
        ConfigInvalid = 600,      ///< 配置值无效（Schema 校验失败）
        ConfigMissing = 601,      ///< 配置键缺失
        ConfigParseFailed = 602,  ///< 配置文件解析失败

        // === 流式类（7xx）===
        StreamError = 700,      ///< 流式传输错误（SSE 解析失败、连接中断）
        StreamCancelled = 701,  ///< 流式传输被取消

        // === 未知 ===
        Unknown = 999,
    } code = Code::Unknown;

    std::string message;  ///< 人类可读错误描述
    std::string context;  ///< 额外上下文（URL / 请求 ID / 工具名 / 配置键）

    /// @brief 默认构造（Code::Unknown）
    Error() = default;

    /// @brief 从错误码构造（message 和 context 为空）
    /// @note 允许隐式转换以支持 Error{Code::X} 简洁语法
    Error(Code c) : code(c) {}

    /// @brief 全字段构造
    Error(Code c, std::string msg, std::string ctx = {})
        : code(c), message(std::move(msg)), context(std::move(ctx)) {}

    /// @brief 是否可重试
    /// @details 网络超时、HTTP 429/5xx、临时不可达可重试；权限/输入错误不可重试
    [[nodiscard]] bool is_retryable() const noexcept {
        switch (code) {
            case Code::NetworkTimeout:
            case Code::NetworkDisconnected:
            case Code::NetworkUnreachable:
            case Code::HttpRateLimited:
            case Code::HttpServerDown:
            case Code::StreamError:
                return true;
            default:
                return false;
        }
    }

    /// @brief 是否为网络错误（HTTP 之外的传输层错误）
    [[nodiscard]] bool is_network_error() const noexcept {
        return code >= Code::NetworkTimeout && code <= Code::NetworkUnreachable;
    }

    /// @brief 是否为 HTTP 错误
    [[nodiscard]] bool is_http_error() const noexcept {
        return code >= Code::HttpError && code <= Code::HttpServerDown;
    }

    /// @brief 是否为客户端错误（通常不可重试）
    [[nodiscard]] bool is_client_error() const noexcept {
        return code == Code::PermissionDenied || code == Code::AuthenticationFailed ||
               code == Code::ResourceNotFound;
    }

    /// @brief 是否为配置错误
    [[nodiscard]] bool is_config_error() const noexcept {
        return code >= Code::ConfigInvalid && code <= Code::ConfigParseFailed;
    }

    /// @brief 是否为流式错误
    [[nodiscard]] bool is_stream_error() const noexcept {
        return code >= Code::StreamError && code <= Code::StreamCancelled;
    }

    /// @brief 错误码 → 字符串（用于日志）
    [[nodiscard]] std::string_view code_string() const noexcept;

    /// @brief 格式化为完整错误消息（包含 code + message + context）
    /// @details 例如 "[NetworkTimeout] Request timed out after 30000ms
    /// (url=https://api.example.com)"
    [[nodiscard]] std::string to_string() const;

    /// @brief 工厂：从 HTTP 响应构造错误
    /// @details 根据 status_code 和 curl_error 字段映射到对应 Code
    /// @param status_code HTTP 状态码（0 表示网络错误，未到达服务器）
    /// @param body 响应体（4xx/5xx 时可能含错误详情）
    /// @param curl_error curl 错误消息（非空表示传输层错误）
    static Error from_http_response(unsigned int status_code, const std::string& body,
                                    const std::string& curl_error);

    /// @brief 工厂：从 CURLcode 构造网络错误
    /// @param curl_code CURLcode 数值（CURLE_OPERATION_TIMEDOUT=28, CURLE_COULDNT_RESOLVE_HOST=6,
    /// 等）
    /// @param url 关联的 URL（填入 context）
    static Error from_curl_code(int curl_code, const std::string& url);

    /// @brief 工厂：从 std::exception 构造内部错误
    /// @param e 异常对象
    /// @param ctx 额外上下文（函数名/模块名）
    static Error from_exception(const std::exception& e, std::string_view ctx = {});
};

/// @brief Error 比较运算符（主要用于测试断言）
/// @note 仅比较 code；message 和 context 不参与比较
bool operator==(const Error& lhs, const Error& rhs) noexcept;
bool operator!=(const Error& lhs, const Error& rhs) noexcept;

}  // namespace agent
