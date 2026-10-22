/**
 * @file test_mcp_http_transport.cpp
 * @brief HttpMcpTransport / Streamable HTTP 传输单元测试（Issue #27 M3）
 * @details 用 tests/unit/agent/mcp/fake_http_mcp_server.py 作为假 HTTP server，
 *          验证 create_transport 路由、2.0 discover / 1.x initialize（含
 *          Mcp-Session-Id 会话头）、JSON 与 SSE 响应解析、错误路径。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <memory>
#include <string>

#include "agent/mcp/mcp_client.h"
#include "agent/mcp/mcp_config.h"
#include "agent/mcp/mcp_stdio_process.h"
#include "agent/mcp/mcp_transport.h"
#include "helpers/python_command.h"

using namespace agent;
using namespace agent::mcp;
using namespace Catch::Matchers;

namespace {

std::string fake_http_server_path() {
    return (std::filesystem::path(SOURCE_DIR) / "tests" / "unit" / "agent" / "mcp" /
            "fake_http_mcp_server.py")
        .string();
}

/// 启动假 HTTP server，返回 base URL（http://127.0.0.1:<port>）
/// @param proc_out 接收持有 server 进程的 shared_ptr（保持存活）
std::string start_fake_http_server(const std::string& mode,
                                   std::shared_ptr<McpStdioProcess>& proc_out) {
    auto proc = std::make_shared<McpStdioProcess>();
    auto start = proc->start(agent::test::python_command(), {fake_http_server_path()},
                             {{"FAKE_MCP_HTTP_MODE", mode}, {"PYTHONHASHSEED", "0"}});
    REQUIRE(start.is_ok());

    auto line = proc->read_line(10000);
    REQUIRE(line.is_ok());
    REQUIRE_THAT(line.value(), StartsWith("PORT="));
    const std::string port = line.value().substr(5);
    REQUIRE_FALSE(port.empty());

    proc_out = std::move(proc);
    return "http://127.0.0.1:" + port;
}

McpServerConfig make_http_cfg(const std::string& name, const std::string& url) {
    McpServerConfig cfg;
    cfg.name = name;
    cfg.url = url;
    cfg.allow_private = true;  // 测试连接回环地址
    return cfg;
}

}  // namespace

// ============================================================================
// create_transport 路由
// ============================================================================

TEST_CASE("create_transport 按配置路由 stdio/http", "[mcp_http][route]") {
    McpServerConfig stdio_cfg;
    stdio_cfg.name = "s";
    stdio_cfg.command = agent::test::python_command();
    auto stdio_t = create_transport(stdio_cfg);
    REQUIRE(dynamic_cast<StdioMcpTransport*>(stdio_t.get()) != nullptr);

    McpServerConfig http_cfg;
    http_cfg.name = "h";
    http_cfg.url = "http://127.0.0.1:9999/mcp";
    auto http_t = create_transport(http_cfg);
    REQUIRE(dynamic_cast<HttpMcpTransport*>(http_t.get()) != nullptr);
}

// ============================================================================
// 2.0 discover 模式（JSON 响应）
// ============================================================================

TEST_CASE("HttpMcpTransport 2.0 discover 模式工具与资源调用", "[mcp_http][discover]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("discover", proc);

    McpClient client;
    auto ok = client.connect(make_http_cfg("fake-http", url), 15000);
    REQUIRE(ok.is_ok());
    REQUIRE(client.is_connected());
    REQUIRE(client.protocol_version() == "2026-07-28");

    auto tools = client.list_tools();
    REQUIRE(tools.is_ok());
    REQUIRE(tools.value().size() == 2);
    REQUIRE(tools.value()[0].name == "echo");

    auto result = client.call_tool("echo", {{"text", "http"}});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().content[0].text == "echo: http");

    auto resources = client.list_resources();
    REQUIRE(resources.is_ok());
    REQUIRE(resources.value().size() == 1);
    REQUIRE(resources.value()[0].uri == "file:///docs/readme.md");

    auto content = client.read_resource("file:///docs/readme.md");
    REQUIRE(content.is_ok());
    REQUIRE_THAT(content.value()[0].text, ContainsSubstring("# Fake Readme"));

    client.disconnect();
}

// ============================================================================
// 1.x initialize 模式（Mcp-Session-Id 会话头）
// ============================================================================

TEST_CASE("HttpMcpTransport 回退 1.x initialize 并携带会话头", "[mcp_http][legacy]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("legacy", proc);

    McpClient client;
    auto ok = client.connect(make_http_cfg("fake-http", url), 15000);
    REQUIRE(ok.is_ok());
    REQUIRE(client.is_connected());
    // 回退模式：协议版本应为 1.x（非 2.0 默认值）
    REQUIRE(client.protocol_version() == "2025-11-25");

    // 回退模式下工具调用可用（会话头已回传）
    auto result = client.call_tool("echo", {{"text", "legacy"}});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().content[0].text == "echo: legacy");

    client.disconnect();
}

// ============================================================================
// SSE 响应模式
// ============================================================================

TEST_CASE("HttpMcpTransport 解析 text/event-stream 响应", "[mcp_http][sse]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("sse", proc);

    McpClient client;
    auto ok = client.connect(make_http_cfg("fake-http", url), 15000);
    REQUIRE(ok.is_ok());
    REQUIRE(client.is_connected());

    auto result = client.call_tool("add", {{"a", 2}, {"b", 3}});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().content[0].text == "5");

    client.disconnect();
}

// ============================================================================
// M4：资源模板 + 提示词 + 2.0 缓存（HTTP）
// ============================================================================

TEST_CASE("HttpMcpTransport 资源模板与提示词", "[mcp_http][m4]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("discover", proc);

    McpClient client;
    REQUIRE(client.connect(make_http_cfg("fake-http", url), 15000).is_ok());

    auto templates = client.list_resource_templates();
    REQUIRE(templates.is_ok());
    REQUIRE(templates.value().size() == 1);
    REQUIRE(templates.value()[0].uri_template == "git:///{owner}/{repo}/blob/{sha}");

    auto prompts = client.list_prompts();
    REQUIRE(prompts.is_ok());
    REQUIRE(prompts.value().size() == 1);
    REQUIRE(prompts.value()[0].name == "summarize");

    auto messages = client.get_prompt("summarize", {{"topic", "http"}});
    REQUIRE(messages.is_ok());
    REQUIRE_THAT(messages.value()[0].content.value("text", ""), ContainsSubstring("http"));

    client.disconnect();
}

TEST_CASE("HttpMcpTransport 2.0 缓存命中", "[mcp_http][cache]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("cache", proc);

    McpClient client;
    REQUIRE(client.connect(make_http_cfg("fake-http", url), 15000).is_ok());

    // 首次：从 server 拉取（返回 _meta.ttlMs=60000 写入缓存）
    auto first = client.list_tools();
    REQUIRE(first.is_ok());
    REQUIRE(first.value().size() == 1);

    // 第二次：server 返回错误，命中缓存则仍成功
    auto second = client.list_tools();
    REQUIRE(second.is_ok());
    REQUIRE(second.value().size() == 1);

    client.disconnect();
}

// ============================================================================
// 错误路径
// ============================================================================

TEST_CASE("HttpMcpTransport 连接拒绝返回错误", "[mcp_http][error]") {
    McpClient client;
    // 未监听的端口
    auto ok = client.connect(make_http_cfg("ghost", "http://127.0.0.1:1/mcp"), 5000);
    REQUIRE(ok.is_err());
    REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("HttpMcpTransport 非法 URL 拒绝启动", "[mcp_http][error]") {
    McpClient client;
    auto ok = client.connect(make_http_cfg("bad", "ftp://example.com/mcp"), 5000);
    REQUIRE(ok.is_err());
    REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("HttpMcpTransport 未启动时调用返回错误", "[mcp_http][error]") {
    HttpMcpTransport transport(make_http_cfg("idle", "http://127.0.0.1:1/mcp"));
    auto resp = transport.send_request(
        nlohmann::json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}, 1000);
    REQUIRE(resp.is_err());
    REQUIRE(resp.error().code == Error::Code::NetworkDisconnected);
}

// ============================================================================
// 端到端：假 server 启动 + 真实 HTTP 调用
// （原名 "TEMP debug ..." 是排查期留下的临时入口。它验证的是「假 server 真能起来、
//   且 transport 能跑通一次真实 HTTP 往返」——这是其余 HTTP 用例的隐含前提，
//   有价值，故转正为正式用例并去掉 [temp] 标签，见 #109）
// ============================================================================

TEST_CASE("HttpMcpTransport 假 server 启动并上报 PORT", "[mcp_http][spawn]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("discover", proc);
    REQUIRE_THAT(url, StartsWith("http://127.0.0.1:"));
    // 仅验证 server 启动 + PORT 读取，不做 HTTP 调用
}

TEST_CASE("HttpMcpTransport 端到端 discover 调用", "[mcp_http][e2e]") {
    std::shared_ptr<McpStdioProcess> proc;
    const std::string url = start_fake_http_server("discover", proc);

    HttpMcpTransport transport(make_http_cfg("fake-http", url));
    auto start = transport.start();
    REQUIRE(start.is_ok());

    auto resp = transport.send_request(nlohmann::json{{"jsonrpc", "2.0"},
                                                      {"id", 1},
                                                      {"method", "server/discover"},
                                                      {"params", nlohmann::json::object()}},
                                       15000);
    REQUIRE(resp.is_ok());
    REQUIRE(resp.value().contains("result"));
}
