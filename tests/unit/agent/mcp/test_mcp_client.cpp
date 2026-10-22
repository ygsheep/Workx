/**
 * @file test_mcp_client.cpp
 * @brief McpClient 端到端单元测试（Issue #27）
 * @details 用 tests/unit/agent/mcp/fake_mcp_server.py 作为 stdio 假 server，
 *          验证真实子进程管道下的协议协商（discover / initialize 回退）、
 *          tools/list、tools/call、resources/list、resources/read。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <filesystem>
#include <string>

#include "agent/mcp/mcp_client.h"
#include "agent/mcp/mcp_config.h"
#include "helpers/python_command.h"

using namespace agent;
using namespace agent::mcp;
using namespace Catch::Matchers;

namespace {

/// 假 server 的绝对路径（SOURCE_DIR 由 CMake 注入）
std::string fake_server_path() {
    return (std::filesystem::path(SOURCE_DIR) / "tests" / "unit" / "agent" / "mcp" /
            "fake_mcp_server.py")
        .string();
}

McpServerConfig make_stdio_cfg(const std::string& name, const std::string& mode) {
    McpServerConfig cfg;
    cfg.name = name;
    cfg.command = agent::test::python_command();
    cfg.args = {fake_server_path()};
    // PYTHONHASHSEED=0：Windows 下子进程熵初始化失败的已知规避
    cfg.env["FAKE_MCP_MODE"] = mode;
    cfg.env["PYTHONHASHSEED"] = "0";
    return cfg;
}

}  // namespace

// ============================================================================
// 2.0 无状态模式（server/discover 成功）
// ============================================================================

TEST_CASE("McpClient connect 2.0 discover 模式并列出工具", "[mcp_client][discover]") {
    McpClient client;
    auto ok = client.connect(make_stdio_cfg("fake", "discover"), 15000);
    REQUIRE(ok.is_ok());
    REQUIRE(client.is_connected());
    REQUIRE(client.name() == "fake");
    REQUIRE(client.protocol_version() == "2026-07-28");

    auto tools = client.list_tools();
    REQUIRE(tools.is_ok());
    REQUIRE(tools.value().size() == 2);
    REQUIRE(tools.value()[0].name == "echo");
    REQUIRE(tools.value()[1].name == "add");
    REQUIRE(tools.value()[0].input_schema.contains("properties"));

    client.disconnect();
    REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("McpClient call_tool 返回文本结果", "[mcp_client][call]") {
    McpClient client;
    REQUIRE(client.connect(make_stdio_cfg("fake", "discover"), 15000).is_ok());

    auto result = client.call_tool("echo", {{"text", "hello"}});
    REQUIRE(result.is_ok());
    REQUIRE_FALSE(result.value().is_error);
    REQUIRE(result.value().content.size() == 1);
    REQUIRE(result.value().content[0].type == "text");
    REQUIRE(result.value().content[0].text == "echo: hello");

    auto sum = client.call_tool("add", {{"a", 2}, {"b", 3}});
    REQUIRE(sum.is_ok());
    REQUIRE(sum.value().content[0].text == "5");
}

TEST_CASE("McpClient call_tool 透传 isError", "[mcp_client][call]") {
    McpClient client;
    REQUIRE(client.connect(make_stdio_cfg("fake", "discover"), 15000).is_ok());

    auto result = client.call_tool("fail", nlohmann::json::object());
    REQUIRE(result.is_ok());
    REQUIRE(result.value().is_error);
    REQUIRE(result.value().content[0].text == "boom");
}

TEST_CASE("McpClient list_resources / read_resource", "[mcp_client][resource]") {
    McpClient client;
    REQUIRE(client.connect(make_stdio_cfg("fake", "discover"), 15000).is_ok());

    auto resources = client.list_resources();
    REQUIRE(resources.is_ok());
    REQUIRE(resources.value().size() == 1);
    REQUIRE(resources.value()[0].uri == "file:///docs/readme.md");
    REQUIRE(resources.value()[0].name == "readme");
    REQUIRE(resources.value()[0].mime_type == "text/markdown");

    auto content = client.read_resource("file:///docs/readme.md");
    REQUIRE(content.is_ok());
    REQUIRE(content.value().size() == 1);
    REQUIRE_THAT(content.value()[0].text, ContainsSubstring("# Fake Readme"));

    auto missing = client.read_resource("file:///nope.txt");
    REQUIRE(missing.is_ok());
    REQUIRE(missing.value().empty());
}

// ============================================================================
// M4：资源模板 + 提示词（stdio）
// ============================================================================

TEST_CASE("McpClient list_resource_templates", "[mcp_client][templates]") {
    McpClient client;
    REQUIRE(client.connect(make_stdio_cfg("fake", "discover"), 15000).is_ok());

    auto templates = client.list_resource_templates();
    REQUIRE(templates.is_ok());
    REQUIRE(templates.value().size() == 1);
    REQUIRE(templates.value()[0].uri_template == "git:///{owner}/{repo}/blob/{sha}");
    REQUIRE(templates.value()[0].name == "blob");
    REQUIRE(templates.value()[0].mime_type == "text/plain");

    client.disconnect();
}

TEST_CASE("McpClient list_prompts / get_prompt", "[mcp_client][prompts]") {
    McpClient client;
    REQUIRE(client.connect(make_stdio_cfg("fake", "discover"), 15000).is_ok());

    auto prompts = client.list_prompts();
    REQUIRE(prompts.is_ok());
    REQUIRE(prompts.value().size() == 1);
    REQUIRE(prompts.value()[0].name == "summarize");
    REQUIRE(prompts.value()[0].arguments.size() == 2);
    REQUIRE(prompts.value()[0].arguments[0].name == "topic");
    REQUIRE(prompts.value()[0].arguments[0].required);
    REQUIRE_FALSE(prompts.value()[0].arguments[1].required);

    auto messages = client.get_prompt("summarize", {{"topic", "MCP"}});
    REQUIRE(messages.is_ok());
    REQUIRE(messages.value().size() == 1);
    REQUIRE(messages.value()[0].role == "user");
    REQUIRE_THAT(messages.value()[0].content.value("text", ""), ContainsSubstring("MCP"));

    auto missing = client.get_prompt("nope", nlohmann::json::object());
    REQUIRE(missing.is_err());

    client.disconnect();
}

// ============================================================================
// M4：2.0 缓存（stdio，cache 模式）
// ============================================================================

TEST_CASE("McpClient 2.0 缓存：第二次 list_tools 命中缓存", "[mcp_client][cache]") {
    McpClient client;
    REQUIRE(client.connect(make_stdio_cfg("fake", "cache"), 15000).is_ok());

    // 首次：从 server 拉取（返回 _meta.ttlMs=60000 写入缓存）
    auto first = client.list_tools();
    REQUIRE(first.is_ok());
    REQUIRE(first.value().size() == 1);
    REQUIRE(first.value()[0].name == "echo");

    // 第二次：server 会返回错误，命中缓存则仍成功（证明未再次请求 server）
    auto second = client.list_tools();
    REQUIRE(second.is_ok());
    REQUIRE(second.value().size() == 1);
    REQUIRE(second.value()[0].name == "echo");

    client.disconnect();
}

// ============================================================================
// 1.x 回退模式（server/discover 返回 MethodNotFound → initialize 握手）
// ============================================================================

TEST_CASE("McpClient connect 回退 1.x initialize 握手", "[mcp_client][legacy]") {
    McpClient client;
    auto ok = client.connect(make_stdio_cfg("fake", "legacy"), 15000);
    REQUIRE(ok.is_ok());
    REQUIRE(client.is_connected());
    // discover 返回 MethodNotFound 时必须回退 1.x，协议版本为 1.x 而非 2.0 默认值
    REQUIRE(client.protocol_version() == "2025-11-25");

    // 回退模式下工具调用同样可用
    auto result = client.call_tool("echo", {{"text", "legacy"}});
    REQUIRE(result.is_ok());
    REQUIRE(result.value().content[0].text == "echo: legacy");

    client.disconnect();
}

// ============================================================================
// 错误路径
// ============================================================================

TEST_CASE("McpClient 未连接时调用返回 NetworkDisconnected", "[mcp_client][error]") {
    McpClient client;
    auto tools = client.list_tools();
    REQUIRE(tools.is_err());
    REQUIRE(tools.error().code == Error::Code::NetworkDisconnected);

    auto result = client.call_tool("echo", nlohmann::json::object());
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::NetworkDisconnected);
}

TEST_CASE("McpClient 连接不存在的命令返回错误", "[mcp_client][error]") {
    McpClient client;
    McpServerConfig cfg;
    cfg.name = "ghost";
    cfg.command = "definitely-not-a-real-command-xyz";
    auto ok = client.connect(cfg, 5000);
    REQUIRE(ok.is_err());
    REQUIRE_FALSE(client.is_connected());
}
