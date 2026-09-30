/**
 * @file mcp_oauth.cpp
 * @brief OAuth 2.0 客户端实现（Issue #27 M4）
 * @details
 * - client_credentials：POST token_endpoint（grant_type=client_credentials）
 * - authorization_code + PKCE：build_authorization_url → 回环回调收 code →
 *   exchange_code（grant_type=authorization_code + code_verifier）
 * - refresh：grant_type=refresh_token 自动续期
 * - 回环回调：Winsock/POSIX 原始 socket 监听 127.0.0.1 一次 HTTP GET
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_oauth.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include "agent/compact/sha256.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#define _DEFAULT_SOURCE  // getentropy
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace agent::mcp {

namespace {

/// @brief 平台 CSPRNG 填充缓冲区（Windows BCryptGenRandom / POSIX getentropy）
/// @return false 表示 CSPRNG 不可用（调用方应拒绝生成安全随机数）
bool fill_random_bytes(uint8_t* buf, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, buf, static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) ==
           0;
#else
    // getentropy 单次上限 256 字节，循环填充
    size_t done = 0;
    while (done < n) {
        const size_t chunk = std::min<size_t>(n - done, 256);
        if (getentropy(buf + done, chunk) != 0) return false;
        done += chunk;
    }
    return true;
#endif
}

/// @brief URL 解码（%XX → 字节；+ → 空格）
std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(s[i + 1]);
            const int lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}

/// @brief 是否回环主机（127.0.0.1 / localhost / ::1 / [::1]）
bool is_loopback_host(const std::string& host) {
    if (host == "localhost") return true;
    std::string h = host;
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']') {
        h = h.substr(1, h.size() - 2);
    }
    if (h == "::1") return true;
    if (h.rfind("127.", 0) == 0) return true;
    return false;
}

/// @brief 从 JSON 响应解析 token 字段并填充成员
/// @return ok: 已填充；err: 响应缺少 access_token
ResultV2<void> parse_token_response(McpOAuthClient* /*unused*/, const nlohmann::json& j,
                                    std::string& access_token, std::string& refresh_token,
                                    std::chrono::steady_clock::time_point& expires_at) {
    if (!j.is_object() || !j.contains("access_token")) {
        return ResultV2<void>::err(
            Error::Code::InvalidInput, "OAuth token 响应缺少 access_token",
            "body=" + (j.is_object() ? j.dump() : std::string("<non-object>")));
    }
    access_token = j.at("access_token").get<std::string>();
    refresh_token = j.value("refresh_token", "");
    const int64_t expires_in = j.value("expires_in", 3600);
    expires_at =
        std::chrono::steady_clock::now() + std::chrono::seconds(expires_in > 0 ? expires_in : 3600);
    return ResultV2<void>::ok();
}

}  // anonymous namespace

ResultV2<void> McpOAuthClient::configure(const McpOAuthConfig& cfg) {
    if (!cfg.valid()) {
        return ResultV2<void>::err(Error::Code::ConfigInvalid,
                                   "OAuth 配置无效：需要 tokenEndpoint + clientId"
                                   "（authorization_code 还需 authEndpoint）",
                                   "McpOAuthClient::configure");
    }
    // P2-1：redirect_uri 必须为回环地址（授权码经本地回调，防泄露到外部）
    if (cfg.flow == "authorization_code" && !cfg.redirect_uri.empty()) {
        const auto purl = HttpClient::parse_url(cfg.redirect_uri);
        if (purl.host.empty() || !is_loopback_host(purl.host)) {
            return ResultV2<void>::err(
                Error::Code::ConfigInvalid,
                "OAuth redirect_uri 必须为回环地址（127.0.0.1/localhost/::1）",
                "redirect_uri=" + cfg.redirect_uri);
        }
    }
    // P2-3：token_endpoint 必须 HTTPS（回环地址允许 http，用于本地测试）
    {
        const auto purl = HttpClient::parse_url(cfg.token_endpoint);
        if (purl.scheme != "https" && !is_loopback_host(purl.host)) {
            return ResultV2<void>::err(Error::Code::ConfigInvalid,
                                       "OAuth token_endpoint 必须使用 HTTPS（回环地址除外）",
                                       "token_endpoint=" + cfg.token_endpoint);
        }
    }
    m_cfg = cfg;
    m_configured = true;
    // 重新配置后失效旧 token
    m_access_token.clear();
    m_refresh_token.clear();
    return ResultV2<void>::ok();
}

ResultV2<std::string> McpOAuthClient::access_token() {
    if (!m_configured) {
        return ResultV2<std::string>::err(Error::Code::ConfigInvalid, "OAuth 未配置",
                                          "McpOAuthClient::access_token");
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_access_token.empty() && m_expires_at > now) {
        return ResultV2<std::string>::ok(m_access_token);
    }
    // 有 refresh_token 先尝试刷新，否则按流程重新获取
    if (!m_refresh_token.empty()) {
        auto r = refresh();
        if (r.is_ok()) return ResultV2<std::string>::ok(m_access_token);
        // 刷新失败（refresh_token 失效）→ 回退重新走完整流程
    }
    if (m_cfg.flow == "client_credentials") {
        auto r = fetch_token("client_credentials", {});
        if (r.is_err()) return r.error();
        return ResultV2<std::string>::ok(m_access_token);
    }
    // authorization_code：无有效 token，需重新授权
    return ResultV2<std::string>::err(Error::Code::PermissionDenied,
                                      "OAuth 需要重新授权（authorization_code 流程）",
                                      "McpOAuthClient::access_token");
}

ResultV2<std::string> McpOAuthClient::authorization_header() {
    auto tok = access_token();
    if (tok.is_err()) return tok.error();
    return ResultV2<std::string>::ok("Bearer " + tok.value());
}

ResultV2<std::string> McpOAuthClient::build_authorization_url() {
    if (!m_configured || m_cfg.flow != "authorization_code") {
        return ResultV2<std::string>::err(Error::Code::ConfigInvalid,
                                          "authorization_code 流程未配置",
                                          "McpOAuthClient::build_authorization_url");
    }
    // PKCE：code_verifier（43-128 字符）→ code_challenge = base64url(SHA256(verifier))
    m_code_verifier = random_string(64);
    const std::string challenge = base64url_encode(compact::sha256(m_code_verifier));
    m_state = random_string(24);

    std::string url = m_cfg.auth_endpoint;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "response_type=code";
    url += "&client_id=" + url_encode(m_cfg.client_id);
    url += "&redirect_uri=" + url_encode(m_cfg.redirect_uri);
    url += "&code_challenge=" + url_encode(challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + url_encode(m_state);
    if (!m_cfg.scopes.empty()) {
        std::string scope;
        for (const auto& s : m_cfg.scopes) {
            if (!scope.empty()) scope += " ";
            scope += s;
        }
        url += "&scope=" + url_encode(scope);
    }
    return ResultV2<std::string>::ok(std::move(url));
}

ResultV2<void> McpOAuthClient::exchange_code(const std::string& code, const std::string& state) {
    if (!m_configured || m_cfg.flow != "authorization_code") {
        return ResultV2<void>::err(Error::Code::ConfigInvalid, "authorization_code 流程未配置",
                                   "McpOAuthClient::exchange_code");
    }
    // RFC 6749 10.12：state 必须与发起授权时一致，否则拒绝（CSRF 防护）
    if (m_state.empty() || state != m_state) {
        return ResultV2<void>::err(Error::Code::PermissionDenied,
                                   "OAuth 回调 state 不匹配，拒绝换取 token（可能的 CSRF 攻击）",
                                   "McpOAuthClient::exchange_code");
    }
    return fetch_token("authorization_code", {
                                                 {"code", code},
                                                 {"redirect_uri", m_cfg.redirect_uri},
                                                 {"code_verifier", m_code_verifier},
                                             });
}

// ============================================================
// 内部
// ============================================================

ResultV2<void> McpOAuthClient::fetch_token(const std::string& grant_type,
                                           const std::map<std::string, std::string>& extra) {
    std::string body =
        "grant_type=" + url_encode(grant_type) + "&client_id=" + url_encode(m_cfg.client_id);
    if (!m_cfg.client_secret.empty()) {
        body += "&client_secret=" + url_encode(m_cfg.client_secret);
    }
    for (const auto& [k, v] : extra) {
        body += "&" + url_encode(k) + "=" + url_encode(v);
    }
    if (!m_cfg.scopes.empty()) {
        std::string scope;
        for (const auto& s : m_cfg.scopes) {
            if (!scope.empty()) scope += " ";
            scope += s;
        }
        body += "&scope=" + url_encode(scope);
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept", "application/json"},
    };
    auto resp = m_http.post(m_cfg.token_endpoint, headers, body, 15000);
    if (resp.is_err()) return resp.error();
    const auto& r = resp.value();
    if (!r.is_success()) {
        return ResultV2<void>::err(
            Error::Code::NetworkDisconnected,
            "OAuth token 请求失败: HTTP " + std::to_string(r.status_code),
            "url=" + m_cfg.token_endpoint + "; body=" + r.body.substr(0, 200));
    }
    try {
        const auto j = nlohmann::json::parse(r.body);
        return parse_token_response(this, j, m_access_token, m_refresh_token, m_expires_at);
    } catch (const nlohmann::json::exception&) {
        return ResultV2<void>::err(Error::Code::InvalidInput, "OAuth token 响应非 JSON",
                                   "body=" + r.body.substr(0, 200));
    }
}

ResultV2<void> McpOAuthClient::refresh() {
    return fetch_token("refresh_token", {
                                            {"refresh_token", m_refresh_token},
                                        });
}

std::string McpOAuthClient::url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string McpOAuthClient::random_string(size_t n) {
    static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    const size_t chars_len = std::strlen(chars);
    // 拒绝采样消除取模偏差（chars_len=66 不整除 256）
    const uint8_t limit = static_cast<uint8_t>(256 - (256 % chars_len));
    std::vector<uint8_t> buf(n * 2);  // 预留余量，避免频繁重填
    if (!fill_random_bytes(buf.data(), buf.size())) return {};
    std::string out;
    out.reserve(n);
    size_t j = 0;
    while (out.size() < n) {
        if (j >= buf.size()) {
            if (!fill_random_bytes(buf.data(), buf.size())) return {};
            j = 0;
        }
        const uint8_t b = buf[j++];
        if (b >= limit) continue;  // 拒绝，消除偏差
        out += chars[b % chars_len];
    }
    return out;
}

std::string McpOAuthClient::base64url_encode(const std::string& in) {
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
        out += table[n & 0x3F];
        i += 3;
    }
    const size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t n = static_cast<uint8_t>(in[i]) << 16;
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
    } else if (rem == 2) {
        const uint32_t n =
            (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
    }
    return out;
}

// ============================================================
// 回环回调监听（authorization_code 流程）
// ============================================================

ResultV2<OAuthCallback> oauth_loopback_listen(int port, int& out_port, int timeout_ms) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return ResultV2<OAuthCallback>::err(Error::Code::InternalError, "WSAStartup 失败",
                                            "oauth_loopback_listen");
    }
    auto cleanup_wsa = [&] { WSACleanup(); };
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd == -1) {
        cleanup_wsa();
        return ResultV2<OAuthCallback>::err(Error::Code::InternalError, "创建 socket 失败",
                                            "oauth_loopback_listen");
    }
    auto close_fd = [&] { closesocket(static_cast<SOCKET>(fd)); };
#else
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        return ResultV2<OAuthCallback>::err(Error::Code::InternalError, "创建 socket 失败",
                                            "oauth_loopback_listen");
    }
    auto close_fd = [&] { ::close(fd); };
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_fd();
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<OAuthCallback>::err(Error::Code::InternalError, "绑定回环端口失败",
                                            "oauth_loopback_listen");
    }
    if (listen(fd, 1) != 0) {
        close_fd();
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<OAuthCallback>::err(Error::Code::InternalError, "监听失败",
                                            "oauth_loopback_listen");
    }

    // 获取实际端口（port=0 时）
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        out_port = ntohs(bound.sin_port);
    }

    // 等待连接（带超时）
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set rfds;
    FD_ZERO(&rfds);
#ifdef _WIN32
    FD_SET(static_cast<SOCKET>(fd), &rfds);
#else
    FD_SET(fd, &rfds);
#endif
    const int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        close_fd();
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<OAuthCallback>::err(Error::Code::NetworkTimeout, "等待 OAuth 回调超时",
                                            "oauth_loopback_listen");
    }

#ifdef _WIN32
    const int client = static_cast<int>(accept(fd, nullptr, nullptr));
#else
    const int client = static_cast<int>(accept(fd, nullptr, nullptr));
#endif
    close_fd();
    if (client < 0) {
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<OAuthCallback>::err(Error::Code::InternalError, "接受回调连接失败",
                                            "oauth_loopback_listen");
    }
    auto close_client = [&] {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(client));
#else
        ::close(client);
#endif
    };

    // 读取请求行（GET /callback?code=...&state=... HTTP/1.1）
    std::string request;
    char buf[1024];
    for (int i = 0; i < 32; ++i) {
        const int n = static_cast<int>(
#ifdef _WIN32
            recv(static_cast<SOCKET>(client), buf, sizeof(buf), 0));
#else
            recv(client, buf, sizeof(buf), 0));
#endif
        if (n <= 0) break;
        request.append(buf, static_cast<size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }

    // 提取 code / state 查询参数（URL 解码）
    OAuthCallback cb;
    const auto qpos = request.find('?');
    if (qpos != std::string::npos) {
        const auto sp = request.find(' ', qpos);
        const std::string query =
            request.substr(qpos + 1, sp == std::string::npos ? std::string::npos : sp - qpos - 1);
        auto query_value = [&query](const std::string& key) -> std::string {
            const std::string needle = key + "=";
            const auto kpos = query.find(needle);
            if (kpos == std::string::npos) return {};
            const auto end = query.find('&', kpos + needle.size());
            const std::string raw = query.substr(
                kpos + needle.size(),
                end == std::string::npos ? std::string::npos : end - kpos - needle.size());
            return url_decode(raw);
        };
        cb.code = query_value("code");
        cb.state = query_value("state");
    }

    // 返回成功页面
    const std::string ok =
        cb.code.empty()
            ? "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            : "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
              "Connection: close\r\n\r\n"
              "<html><body><h3>授权成功，可关闭此窗口</h3></body></html>";
#ifdef _WIN32
    send(static_cast<SOCKET>(client), ok.data(), static_cast<int>(ok.size()), 0);
#else
    ::send(client, ok.data(), ok.size(), 0);
#endif
    close_client();
#ifdef _WIN32
    cleanup_wsa();
#endif

    if (cb.code.empty()) {
        return ResultV2<OAuthCallback>::err(Error::Code::InvalidInput, "OAuth 回调缺少 code 参数",
                                            "oauth_loopback_listen");
    }
    return ResultV2<OAuthCallback>::ok(std::move(cb));
}

}  // namespace agent::mcp
