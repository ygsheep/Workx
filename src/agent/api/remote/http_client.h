#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <memory>
#include <chrono>

#include <nlohmann/json.hpp>

#include "core/utils/result.h"     // 旧 Result（过渡期保留）
#include "core/utils/result_v2.h"  // V2-2：新 ResultV2
#include "core/utils/error.h"      // V2-2：Error 类型

namespace agent {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

/// @brief HTTP 响应（E-6：错误码分类清晰化）
/// @details
/// 字段语义：
/// - `status_code`：HTTP 状态码；网络错误时为 0
/// - `body`：响应体（即使 4xx/5xx 也可能有 body）
/// - `error`：curl/网络错误描述；HTTP 错误时为空
///
/// 三种状态判断：
/// | 状态         | status_code | error    | 便捷方法              |
/// |--------------|-------------|----------|-----------------------|
/// | 成功 (2xx)   | 200-299     | empty    | is_success()          |
/// | HTTP 错误    | 4xx/5xx     | empty    | is_http_error()       |
/// | 网络错误     | 0           | non-empty| is_network_error()    |
struct HttpResponse {
    unsigned int status_code = 0;
    std::string body;
    std::string error;
    /// 响应头（键已转小写，供 Content-Type / Mcp-Session-Id 等读取）
    std::vector<std::pair<std::string, std::string>> headers;

    /// @brief 是否成功（HTTP 2xx）
    bool is_success() const noexcept {
        return error.empty() && status_code >= 200 && status_code <= 299;
    }

    /// @brief 是否 HTTP 错误（4xx/5xx，curl 无错误）
    bool is_http_error() const noexcept {
        return error.empty() && status_code >= 400 && status_code <= 599;
    }

    /// @brief 是否网络错误（curl 失败，无 HTTP 响应）
    bool is_network_error() const noexcept { return !error.empty() && status_code == 0; }

    /// @brief 是否客户端错误（4xx）
    bool is_client_error() const noexcept {
        return error.empty() && status_code >= 400 && status_code <= 499;
    }

    /// @brief 是否服务端错误（5xx，可重试）
    bool is_server_error() const noexcept {
        return error.empty() && status_code >= 500 && status_code <= 599;
    }

    /// @brief 是否限流（429 Too Many Requests）
    bool is_rate_limited() const noexcept { return error.empty() && status_code == 429; }

    /// @brief 是否可重试错误（与 HttpRetryPolicy.is_retryable 配对）
    /// @details 429 + 5xx + 网络错误 可重试；4xx（除 429）不可重试
    bool is_retryable() const noexcept {
        return is_rate_limited() || is_server_error() || is_network_error();
    }
};

class SSEStreamReader;

/// @brief HTTP 客户端抽象接口（M-1：可注入，便于 RemoteBackend 并发逻辑直接单测）
/// @details RemoteBackend 通过本接口依赖 HttpClient，测试可注入 Fake 实现
///          驱动 on_complete / cancel_stream 回调，验证多 reader 并发仲裁路径。
class IHttpClient {
   public:
    virtual ~IHttpClient() = default;

    /// @brief 同步 GET 请求
    /// @return 成功返回 HttpResponse（含 2xx/4xx/5xx 状态码）；
    ///         失败返回 Error（仅限网络错误，HTTP 4xx/5xx 仍通过 HttpResponse 返回）
    virtual ResultV2<HttpResponse> get(
        const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
        int timeout_ms = 15000) = 0;

    /// @brief 同步 POST（原始 body）
    /// @details Content-Type 等需要通过 headers 自行设置
    virtual ResultV2<HttpResponse> post(
        const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body, int timeout_ms = 15000) = 0;

    /// @brief 同步 POST JSON（自动追加 Content-Type: application/json）
    /// @param json_body 待发送的 JSON 对象
    ResultV2<HttpResponse> post_json(
        const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
        const nlohmann::json& json_body, int timeout_ms = 15000) {
        auto copy = headers;
        const auto ieq = [](const std::string& a, const char* b) {
            if (a.size() != std::strlen(b)) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(a[i])) !=
                    std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        };
        bool has_ct = false;
        for (const auto& [k, v] : copy) {
            if (ieq(k, "Content-Type")) {
                has_ct = true;
                break;
            }
        }
        if (!has_ct) copy.emplace_back("Content-Type", "application/json");
        return post(url, std::move(copy), json_body.dump(), timeout_ms);
    }

    virtual void async_post_stream(const std::string& url,
                                   const std::vector<std::pair<std::string, std::string>>& headers,
                                   const std::string& body, std::shared_ptr<SSEStreamReader> reader,
                                   std::function<void()> on_complete,
                                   int timeout_ms = 30000) const = 0;

    virtual void cancel_stream(SSEStreamReader* reader) = 0;

    virtual void shutdown() = 0;
};

class HttpClient : public IHttpClient {
   public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    ResultV2<HttpResponse> get(const std::string& url,
                               const std::vector<std::pair<std::string, std::string>>& headers,
                               int timeout_ms = 15000) override;

    ResultV2<HttpResponse> post(const std::string& url,
                                const std::vector<std::pair<std::string, std::string>>& headers,
                                const std::string& body, int timeout_ms = 15000) override;

    void async_post_stream(const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body, std::shared_ptr<SSEStreamReader> reader,
                           std::function<void()> on_complete,
                           int timeout_ms = 30000) const override;

    void cancel_stream(SSEStreamReader* reader) override;

    void shutdown() override;

    static ParsedUrl parse_url(const std::string& url);

    /// @brief 启用 SSRF 防护（#25）：连接目标解析到内网/回环/链路本地地址时拒绝连接
    /// @details 通过 CURLOPT_OPENSOCKETFUNCTION 在建立连接前拦截，
    ///          覆盖重定向（3xx 跟随）后的最终目标地址。默认关闭，
    ///          不影响本地模型服务等内网通信场景；WebFetch 等面向公网的工具开启。
    void set_block_private_ips(bool block) noexcept { m_block_private_ips = block; }
    bool block_private_ips() const noexcept { return m_block_private_ips; }

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    bool m_block_private_ips = false;
};

}  // namespace agent