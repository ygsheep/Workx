/**
 * @file test_mcp_tools.cpp
 * @brief MCP 三件套工具单元测试（Issue #27）
 * @details 覆盖 MCPTool / ListMcpResourcesTool / ReadMcpResourceTool：
 *          元数据、schema、空 manager 错误路径、未连接 server 错误路径、
 *          权限（Bypass 放行）、prompt 注入。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <memory>
#include <string>

#include "agent/tool/MCPTool/mcp_tool.h"
#include "agent/tool/ListMcpResourcesTool/list_mcp_resources_tool.h"
#include "agent/tool/ReadMcpResourceTool/read_mcp_resource_tool.h"
#include "agent/tool/AgentTool/agent_tool.h"
#include "agent/mcp/mcp_client_manager.h"
#include "agent/tool/permission_ask.h"

using namespace agent;
using namespace agent::tool;
using namespace Catch::Matchers;

namespace {

/// ToolContext 含 std::atomic 成员不可拷贝，就地填充
void make_ctx(ToolContext& ctx, PermissionMode mode = PermissionMode::Default) {
    ctx.permission_mode = mode;
    ctx.session_id = "test";
}

}  // namespace

// ============================================================================
// MCPTool 元数据 + schema
// ============================================================================

TEST_CASE("MCPTool 名称/描述/schema 正确", "[mcp_tool][meta]") {
    MCPTool tool(nullptr);
    REQUIRE(tool.name() == "MCP");
    REQUIRE_FALSE(tool.description().empty());

    auto s = tool.input_schema();
    REQUIRE(s.at("type") == "object");
    REQUIRE(s.at("properties").contains("server"));
    REQUIRE(s.at("properties").contains("tool"));
    REQUIRE(s.at("properties").contains("input"));
    const auto& req = s.at("required");
    REQUIRE(std::find(req.begin(), req.end(), "server") != req.end());
    REQUIRE(std::find(req.begin(), req.end(), "tool") != req.end());
}

TEST_CASE("MCPTool prompt 无 server 时提示未连接", "[mcp_tool][prompt]") {
    MCPTool tool(nullptr);
    REQUIRE_THAT(tool.prompt(), ContainsSubstring("当前没有已连接的 MCP server"));
}

// ============================================================================
// MCPTool call 错误路径
// ============================================================================

TEST_CASE("MCPTool call 缺 server/tool 返回 InvalidInput", "[mcp_tool][call]") {
    MCPTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx);

    auto no_server = tool.call(R"({"tool":"echo"})"_json, ctx);
    REQUIRE(no_server.is_err());
    REQUIRE(no_server.error().code == Error::Code::InvalidInput);

    auto no_tool = tool.call(R"({"server":"fake"})"_json, ctx);
    REQUIRE(no_tool.is_err());
    REQUIRE(no_tool.error().code == Error::Code::InvalidInput);
}

TEST_CASE("MCPTool call 空 manager 返回 InternalError", "[mcp_tool][call]") {
    MCPTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({"server":"fake","tool":"echo"})"_json, ctx);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::InternalError);
}

TEST_CASE("MCPTool call 未连接 server 返回 ResourceNotFound", "[mcp_tool][call]") {
    auto manager = std::make_shared<mcp::McpClientManager>();
    MCPTool tool(manager);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({"server":"ghost","tool":"echo"})"_json, ctx);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ResourceNotFound);
    REQUIRE_THAT(result.error().message, ContainsSubstring("ghost"));
}

// ============================================================================
// MCPTool 权限
// ============================================================================

TEST_CASE("MCPTool check_permissions Bypass 模式放行", "[mcp_tool][perm]") {
    MCPTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx, PermissionMode::BypassPermissions);
    auto result = tool.check_permissions(R"({"server":"fake","tool":"echo"})"_json, ctx);
    REQUIRE(result.is_ok());
}

TEST_CASE("MCPTool check_permissions 缺 server 放行", "[mcp_tool][perm]") {
    MCPTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.check_permissions(R"({"tool":"echo"})"_json, ctx);
    REQUIRE(result.is_ok());
}

// ============================================================================
// ListMcpResourcesTool
// ============================================================================

TEST_CASE("ListMcpResourcesTool 元数据 + schema", "[mcp_resources][meta]") {
    ListMcpResourcesTool tool(nullptr);
    REQUIRE(tool.name() == "ListMcpResourcesTool");
    auto s = tool.input_schema();
    REQUIRE(s.at("properties").contains("server"));
}

TEST_CASE("ListMcpResourcesTool 空 manager 返回 InternalError", "[mcp_resources][call]") {
    ListMcpResourcesTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({})"_json, ctx);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::InternalError);
}

TEST_CASE("ListMcpResourcesTool 无已连接 server 返回提示文本", "[mcp_resources][call]") {
    auto manager = std::make_shared<mcp::McpClientManager>();
    ListMcpResourcesTool tool(manager);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({})"_json, ctx);
    REQUIRE(result.is_ok());
    REQUIRE_THAT(result.value().text, ContainsSubstring("没有已连接的 MCP server"));
}

TEST_CASE("ListMcpResourcesTool 指定不存在的 server 返回 ResourceNotFound",
          "[mcp_resources][call]") {
    auto manager = std::make_shared<mcp::McpClientManager>();
    ListMcpResourcesTool tool(manager);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({"server":"ghost"})"_json, ctx);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ResourceNotFound);
}

// ============================================================================
// ReadMcpResourceTool
// ============================================================================

TEST_CASE("ReadMcpResourceTool 元数据 + schema", "[mcp_resource][meta]") {
    ReadMcpResourceTool tool(nullptr);
    REQUIRE(tool.name() == "ReadMcpResourceTool");
    auto s = tool.input_schema();
    REQUIRE(s.at("properties").contains("server"));
    REQUIRE(s.at("properties").contains("uri"));
    const auto& req = s.at("required");
    REQUIRE(std::find(req.begin(), req.end(), "uri") != req.end());
}

TEST_CASE("ReadMcpResourceTool 缺 server/uri 返回 InvalidInput", "[mcp_resource][call]") {
    ReadMcpResourceTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx);

    auto no_server = tool.call(R"({"uri":"file:///x"})"_json, ctx);
    REQUIRE(no_server.is_err());
    REQUIRE(no_server.error().code == Error::Code::InvalidInput);

    auto no_uri = tool.call(R"({"server":"fake"})"_json, ctx);
    REQUIRE(no_uri.is_err());
    REQUIRE(no_uri.error().code == Error::Code::InvalidInput);
}

TEST_CASE("ReadMcpResourceTool 空 manager 返回 InternalError", "[mcp_resource][call]") {
    ReadMcpResourceTool tool(nullptr);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({"server":"fake","uri":"file:///x"})"_json, ctx);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::InternalError);
}

TEST_CASE("ReadMcpResourceTool 未连接 server 返回 ResourceNotFound", "[mcp_resource][call]") {
    auto manager = std::make_shared<mcp::McpClientManager>();
    ReadMcpResourceTool tool(manager);
    ToolContext ctx;
    make_ctx(ctx);
    auto result = tool.call(R"({"server":"ghost","uri":"https://example.com/resource"})"_json, ctx);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ResourceNotFound);
}

TEST_CASE("ReadMcpResourceTool 危险 URI 拒绝（SSRF/本地文件）", "[mcp_resource][call][ssrf]") {
    auto manager = std::make_shared<mcp::McpClientManager>();
    ReadMcpResourceTool tool(manager);
    ToolContext ctx;
    make_ctx(ctx);

    // file:// 本地文件读取
    auto file_uri = tool.call(R"({"server":"ghost","uri":"file:///etc/passwd"})"_json, ctx);
    REQUIRE(file_uri.is_err());
    REQUIRE(file_uri.error().code == Error::Code::PermissionDenied);

    // gopher:// 内网探测
    auto gopher_uri =
        tool.call(R"({"server":"ghost","uri":"gopher://169.254.169.254:80/"})"_json, ctx);
    REQUIRE(gopher_uri.is_err());
    REQUIRE(gopher_uri.error().code == Error::Code::PermissionDenied);

    // 内网 http 地址（SSRF）
    auto private_uri = tool.call(
        R"({"server":"ghost","uri":"http://169.254.169.254/latest/meta-data/"})"_json, ctx);
    REQUIRE(private_uri.is_err());
    REQUIRE(private_uri.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("ReadMcpResourceTool check_permissions（P2-7）", "[mcp_resource][perm]") {
    auto manager = std::make_shared<mcp::McpClientManager>();
    ReadMcpResourceTool tool(manager);

    // Bypass 模式放行
    ToolContext ctx_bypass;
    make_ctx(ctx_bypass, PermissionMode::BypassPermissions);
    auto bypass = tool.check_permissions(R"({"server":"ghost","uri":"https://example.com/r"})"_json,
                                         ctx_bypass);
    REQUIRE(bypass.is_ok());

    // Default 模式无确认通道 → fail-closed 拒绝
    ToolContext ctx;
    make_ctx(ctx);
    auto denied =
        tool.check_permissions(R"({"server":"ghost","uri":"https://example.com/r"})"_json, ctx);
    REQUIRE(denied.is_err());
    REQUIRE(denied.error().code == Error::Code::PermissionDenied);

    // 缺 server 放行
    auto no_server = tool.check_permissions(R"({"uri":"https://example.com/r"})"_json, ctx);
    REQUIRE(no_server.is_ok());
}

// ============================================================================
// #56 方案 D：AgentTool mcpServers 作用域构建
// ============================================================================

TEST_CASE("AgentTool build_mcp_scope 空/null servers 返回空作用域", "[agent_tool][mcp_scope]") {
    auto parent = std::make_shared<mcp::McpClientManager>();

    // 空数组 / null → scope 为 nullptr（调用方以 scope 是否非空判定可用性），无 owned client
    auto r1 = AgentTool::build_mcp_scope(R"([])"_json, parent.get());
    REQUIRE(r1.scope == nullptr);
    REQUIRE(r1.owned_clients.empty());

    auto r2 = AgentTool::build_mcp_scope(nullptr, parent.get());
    REQUIRE(r2.scope == nullptr);
    REQUIRE(r2.owned_clients.empty());

    // 非数组输入同样按空处理
    auto r3 = AgentTool::build_mcp_scope(R"({"foo":1})"_json, parent.get());
    REQUIRE(r3.scope == nullptr);
    REQUIRE(r3.owned_clients.empty());
}

TEST_CASE("AgentTool build_mcp_scope 字符串引用复用父 client 不清理", "[agent_tool][mcp_scope]") {
    auto parent = std::make_shared<mcp::McpClientManager>();
    auto ref_client = std::make_shared<mcp::McpClient>();
    parent->register_client("shared", ref_client);

    auto r = AgentTool::build_mcp_scope(R"(["shared"])"_json, parent.get());

    REQUIRE(r.scope != nullptr);
    REQUIRE_FALSE(r.scope->empty());
    REQUIRE(r.owned_clients.empty());                      // 引用不复用 ∴ 无 owned client
    REQUIRE(r.scope->get_client("shared") == ref_client);  // 同一实例被复用
}

TEST_CASE("AgentTool build_mcp_scope 未知引用/无父管理器时静默跳过", "[agent_tool][mcp_scope]") {
    auto parent = std::make_shared<mcp::McpClientManager>();

    // 未知引用名 → 跳过（不注册、不产物）
    auto unknown = AgentTool::build_mcp_scope(R"(["ghost1"])"_json, parent.get());
    REQUIRE(unknown.scope != nullptr);
    REQUIRE(unknown.scope->empty());
    REQUIRE(unknown.owned_clients.empty());

    // 父管理器为 nullptr → 所有引用均跳过
    auto null_parent = AgentTool::build_mcp_scope(R"(["ghost2"])"_json, nullptr);
    REQUIRE(null_parent.owned_clients.empty());
}

TEST_CASE("AgentTool build_mcp_scope inline 连接失败静默跳过（异常安全）",
          "[agent_tool][mcp_scope]") {
    auto parent = std::make_shared<mcp::McpClientManager>();
    // inline 对象指向不存在的命令：connect_one_off 失败 → 静默跳过，不抛异常不挂起
    nlohmann::json servers = nlohmann::json::array({{{"name", "inline_bad"},
                                                     {"command", "no_such_command_xyz"},
                                                     {"args", nlohmann::json::array()}}});
    auto r = AgentTool::build_mcp_scope(servers, parent.get());
    REQUIRE(r.scope != nullptr);
    REQUIRE(r.scope->empty());         // 连接失败未注册
    REQUIRE(r.owned_clients.empty());  // 失败不产生 owned client
}

TEST_CASE("AgentTool build_mcp_scope 混合引用与 inline 仅引用可见", "[agent_tool][mcp_scope]") {
    auto parent = std::make_shared<mcp::McpClientManager>();
    auto ref_client = std::make_shared<mcp::McpClient>();
    parent->register_client("shared", ref_client);

    nlohmann::json servers = nlohmann::json::array({
        "shared",  // 引用：复用父 client
        {{"name", "inline_bad"},
         {"command", "no_such_command_xyz"},
         {"args", nlohmann::json::array()}}  // inline：失败跳过
    });
    auto r = AgentTool::build_mcp_scope(servers, parent.get());
    REQUIRE(r.scope != nullptr);
    REQUIRE_FALSE(r.scope->empty());  // 引用可用 → scope 非空
    REQUIRE(r.scope->get_client("shared") == ref_client);
    REQUIRE(r.owned_clients.empty());  // 仅引用成功，无 owned client
}
