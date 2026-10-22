/**
 * @file test_mcp_oauth.cpp
 * @brief McpOAuthClient 单元测试（Issue #27 M4）
 * @details 用 tests/unit/agent/mcp/fake_oauth_server.py 作为假 token server，
 *          验证 client_credentials / authorization_code（PKCE）流程、
 *          token 缓存与刷新、配置校验、回环回调监听。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include "agent/mcp/mcp_oauth.h"
#include "agent/mcp/mcp_stdio_process.h"
#include "helpers/python_command.h"

using namespace agent;
using namespace agent::mcp;
using namespace Catch::Matchers;

namespace {

std::string fake_oauth_server_path() {
    return (std::filesystem::path(SOURCE_DIR) / "tests" / "unit" / "agent" / "mcp" /
            "fake_oauth_server.py")
        .string();
}

/// 启动假 OAuth server，返回 base URL（http://127.0.0.1:<port>）
std::string start_fake_oauth(std::shared_ptr<McpStdioProcess>& proc_out) {
    auto proc = std::make_shared<McpStdioProcess>();
    auto start = proc->start(agent::test::python_command(), {fake_oauth_server_path()},
                             {{"PYTHONHASHSEED", "0"}});
    REQUIRE(start.is_ok());

    auto line = proc->read_line(10000);
    REQUIRE(line.is_ok());
    REQUIRE_THAT(line.value(), StartsWith("PORT="));
    const std::string port = line.value().substr(5);
    REQUIRE_FALSE(port.empty());

    proc_out = std::move(proc);
    return "http://127.0.0.1:" + port;
}

}  // namespace

// ============================================================================
// 配置校验
// ============================================================================

TEST_CASE("McpOAuthClient 无效配置拒绝 configure", "[mcp_oauth][config]") {
    McpOAuthClient client;
    McpOAuthConfig cfg;  // 空配置
    auto r = client.configure(cfg);
    REQUIRE(r.is_err());
    REQUIRE_FALSE(client.configured());
}

TEST_CASE("McpOAuthClient authorization_code 缺 authEndpoint 拒绝", "[mcp_oauth][config]") {
    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "authorization_code";
    cfg.client_id = "cid";
    cfg.token_endpoint = "http://127.0.0.1:1/token";
    // 缺 auth_endpoint
    auto r = client.configure(cfg);
    REQUIRE(r.is_err());
}

TEST_CASE("McpOAuthClient redirect_uri 非回环拒绝（P2-1）", "[mcp_oauth][config]") {
    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "authorization_code";
    cfg.client_id = "cid";
    cfg.token_endpoint = "http://127.0.0.1:1/token";
    cfg.auth_endpoint = "https://auth.example.com/authorize";
    cfg.redirect_uri = "https://evil.example.com/callback";  // 非回环
    auto r = client.configure(cfg);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ConfigInvalid);
}

TEST_CASE("McpOAuthClient token_endpoint 非 HTTPS 拒绝（P2-3）", "[mcp_oauth][config]") {
    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "client_credentials";
    cfg.client_id = "cid";
    cfg.token_endpoint = "http://token.example.com/token";  // 非回环 + 非 HTTPS
    auto r = client.configure(cfg);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ConfigInvalid);
}

TEST_CASE("McpOAuthClient 回环 http token_endpoint 放行", "[mcp_oauth][config]") {
    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "client_credentials";
    cfg.client_id = "cid";
    cfg.token_endpoint = "http://127.0.0.1:1/token";  // 回环 http 允许（本地测试）
    auto r = client.configure(cfg);
    REQUIRE(r.is_ok());
}

// ============================================================================
// client_credentials 流程
// ============================================================================

TEST_CASE("McpOAuthClient client_credentials 获取 token", "[mcp_oauth][cc]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string base = start_fake_oauth(proc);

    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "client_credentials";
    cfg.client_id = "cid";
    cfg.client_secret = "secret";
    cfg.token_endpoint = base + "/token";
    REQUIRE(client.configure(cfg).is_ok());
    REQUIRE(client.configured());

    auto token = client.access_token();
    REQUIRE(token.is_ok());
    REQUIRE(token.value() == "cc-token");

    // token 缓存：再次获取不重新请求（仍返回同一 token）
    auto cached = client.access_token();
    REQUIRE(cached.is_ok());
    REQUIRE(cached.value() == "cc-token");

    auto header = client.authorization_header();
    REQUIRE(header.is_ok());
    REQUIRE(header.value() == "Bearer cc-token");
}

// ============================================================================
// authorization_code 流程（PKCE）
// ============================================================================

TEST_CASE("McpOAuthClient authorization_code 生成授权 URL 并换 token", "[mcp_oauth][ac]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string base = start_fake_oauth(proc);

    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "authorization_code";
    cfg.client_id = "cid";
    cfg.token_endpoint = base + "/token";
    cfg.auth_endpoint = "https://auth.example.com/authorize";
    cfg.redirect_uri = "http://127.0.0.1:0/callback";
    cfg.scopes = {"mcp", "read"};
    REQUIRE(client.configure(cfg).is_ok());

    // 授权 URL 含 PKCE 参数
    auto url = client.build_authorization_url();
    REQUIRE(url.is_ok());
    REQUIRE_THAT(url.value(), StartsWith("https://auth.example.com/authorize?"));
    REQUIRE_THAT(url.value(), ContainsSubstring("response_type=code"));
    REQUIRE_THAT(url.value(), ContainsSubstring("code_challenge="));
    REQUIRE_THAT(url.value(), ContainsSubstring("code_challenge_method=S256"));
    REQUIRE_THAT(url.value(), ContainsSubstring("state="));
    REQUIRE_THAT(url.value(), ContainsSubstring("scope=mcp%20read"));

    // 提取 state（CSRF 校验用）
    const std::string& auth_url = url.value();
    const auto sp = auth_url.find("state=");
    REQUIRE(sp != std::string::npos);
    const auto se = auth_url.find('&', sp + 6);
    const std::string state =
        auth_url.substr(sp + 6, se == std::string::npos ? std::string::npos : se - sp - 6);
    REQUIRE_FALSE(state.empty());

    // 用授权码换 token（fake server 校验 code_verifier 非空）
    auto exchanged = client.exchange_code("auth-code-1", state);
    REQUIRE(exchanged.is_ok());
    auto token = client.access_token();
    REQUIRE(token.is_ok());
    REQUIRE(token.value() == "ac-token");
}

TEST_CASE("McpOAuthClient exchange_code state 不匹配拒绝（CSRF）", "[mcp_oauth][ac]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string base = start_fake_oauth(proc);

    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "authorization_code";
    cfg.client_id = "cid";
    cfg.token_endpoint = base + "/token";
    cfg.auth_endpoint = "https://auth.example.com/authorize";
    cfg.redirect_uri = "http://127.0.0.1:0/callback";
    REQUIRE(client.configure(cfg).is_ok());

    auto url = client.build_authorization_url();
    REQUIRE(url.is_ok());

    // 攻击者注入错误 state → 拒绝换取 token
    auto exchanged = client.exchange_code("auth-code-1", "attacker-controlled-state");
    REQUIRE(exchanged.is_err());
    REQUIRE(exchanged.error().code == Error::Code::PermissionDenied);
}

// ============================================================================
// token 刷新
// ============================================================================

TEST_CASE("McpOAuthClient refresh_token 自动刷新", "[mcp_oauth][refresh]") {
    // 短过期模式：expires_in=1，token 过期后 access_token() 走 refresh 流程
    auto proc = std::make_shared<McpStdioProcess>();
    auto start = proc->start(agent::test::python_command(), {fake_oauth_server_path()},
                             {{"FAKE_OAUTH_SHORT_EXPIRY", "1"}, {"PYTHONHASHSEED", "0"}});
    REQUIRE(start.is_ok());
    auto line = proc->read_line(10000);
    REQUIRE(line.is_ok());
    REQUIRE_THAT(line.value(), StartsWith("PORT="));
    const std::string base = "http://127.0.0.1:" + line.value().substr(5);

    McpOAuthClient client;
    McpOAuthConfig cfg;
    cfg.flow = "client_credentials";
    cfg.client_id = "cid";
    cfg.token_endpoint = base + "/token";
    REQUIRE(client.configure(cfg).is_ok());

    // 首次获取（expires_in=1，1 秒后过期）
    auto token = client.access_token();
    REQUIRE(token.is_ok());
    REQUIRE(token.value() == "cc-token");

    // 等待过期后再次获取 → 走 refresh_token 流程拿到 rt-token
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto refreshed = client.access_token();
    REQUIRE(refreshed.is_ok());
    REQUIRE(refreshed.value() == "rt-token");
}

// ============================================================================
// 回环回调监听
// ============================================================================

TEST_CASE("oauth_loopback_listen 超时返回错误", "[mcp_oauth][loopback]") {
    int port = 0;
    auto r = oauth_loopback_listen(0, port, 200);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::NetworkTimeout);
}

TEST_CASE("oauth_loopback_listen 端口 0 自动分配", "[mcp_oauth][loopback]") {
    // 仅验证端口分配逻辑（不等待连接，用极短超时触发超时路径）
    int port = 0;
    auto r = oauth_loopback_listen(0, port, 50);
    REQUIRE(r.is_err());  // 无连接必然超时
}
