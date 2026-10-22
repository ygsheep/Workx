/**
 * @file mcp_oauth.h
 * @brief OAuth 2.0 客户端（Issue #27 M4）
 * @details 为 MCP HTTP 传输提供 OAuth 2.0 认证：
 *          - client_credentials 流程：机器间直接换取 token（无需用户交互）
 *          - authorization_code 流程（PKCE）：生成授权 URL → 回环回调
 *            接收授权码 → 换 token；token 过期自动 refresh
 *          - token 缓存：过期前复用，过期后刷新，避免重复授权
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "agent/api/remote/http_client.h"
#include "core/utils/result_v2.h"

namespace agent::mcp {

/// @brief OAuth 2.0 配置（mcp.json 的 server.oauth 段）
struct McpOAuthConfig {
    std::string flow;  ///< "client_credentials" | "authorization_code"
    std::string client_id;
    std::string client_secret;
    std::string token_endpoint;  ///< token 端点 URL
    std::string auth_endpoint;   ///< 授权端点 URL（authorization_code 用）
    std::vector<std::string> scopes;
    std::string redirect_uri;  ///< 回调地址（authorization_code；默认回环自动端口）

    /// 是否有效（client_credentials 需 token_endpoint + client_id）
    bool valid() const {
        if (token_endpoint.empty() || client_id.empty()) return false;
        if (flow == "authorization_code" && auth_endpoint.empty()) return false;
        return true;
    }
};

/// @brief OAuth 2.0 客户端（M4）
/// @details 线程安全由调用方保证（HttpMcpTransport 单线程使用）。
class McpOAuthClient {
   public:
    /// @brief 配置 OAuth 参数（幂等，可重复配置覆盖）
    ResultV2<void> configure(const McpOAuthConfig& cfg);

    /// @brief 获取有效 access token（未获取或过期时自动获取/刷新）
    /// @return ok: token 字符串；err: 获取失败
    ResultV2<std::string> access_token();

    /// @brief Authorization 头值（"Bearer <token>"）；未配置返回空字符串
    ResultV2<std::string> authorization_header();

    /// @brief 生成授权 URL（authorization_code + PKCE）
    /// @details 需在 configure 后调用；返回 URL 供用户浏览器打开
    ResultV2<std::string> build_authorization_url();

    /// @brief 用授权码换取 token（authorization_code 流程）
    /// @param code 回调收到的授权码
    /// @param state 回调收到的 state；必须与 build_authorization_url 生成的一致，
    ///              否则拒绝（RFC 6749 10.12 CSRF 防护）
    ResultV2<void> exchange_code(const std::string& code, const std::string& state);

    bool configured() const { return m_configured; }

   private:
    ResultV2<void> fetch_token(const std::string& grant_type,
                               const std::map<std::string, std::string>& extra);
    ResultV2<void> refresh();

    static std::string url_encode(const std::string& s);
    static std::string random_string(size_t n);
    static std::string base64url_encode(const std::string& in);

    McpOAuthConfig m_cfg;
    HttpClient m_http;
    std::string m_access_token;
    std::string m_refresh_token;
    std::string m_code_verifier;  ///< PKCE code_verifier（授权码换 token 用）
    std::string m_state;          ///< CSRF state（回调校验用）
    std::chrono::steady_clock::time_point m_expires_at;
    bool m_configured = false;
};

/// @brief 回环回调解析结果（authorization_code 流程）
struct OAuthCallback {
    std::string code;   ///< 授权码
    std::string state;  ///< CSRF state（须与发起授权时一致）
};

/// @brief 在回环地址监听一次 HTTP 回调，提取授权码与 state（authorization_code 流程）
/// @param port 监听端口（0=自动分配，经 out_port 返回实际端口）
/// @param out_port 输出实际监听端口（port=0 时）
/// @param timeout_ms 等待超时（毫秒）
/// @return ok: 回调参数（code + state）；err: 超时/监听失败
/// @details 返回前已向浏览器返回成功页面并关闭连接。
ResultV2<OAuthCallback> oauth_loopback_listen(int port, int& out_port, int timeout_ms);

}  // namespace agent::mcp
