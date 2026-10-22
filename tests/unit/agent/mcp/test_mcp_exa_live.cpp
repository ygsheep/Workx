/**
 * @file test_mcp_exa_live.cpp
 * @brief Exa MCP 实时集成测试（Issue #27 M4 验证）
 * @details 从真实用户配置 ~/.workx/mcp.json 加载 "exa" server（mcp-remote 桥接
 *          https://mcp.exa.ai/mcp），端到端验证：
 *          - 自适应协议协商（2.0 discover → 1.x initialize 回退）
 *          - tools/list 返回 web_search_exa / web_fetch_exa
 *          - tools/call 真实搜索返回结果
 *
 *          实时测试（依赖外网 + npx），标签 [mcp_exa][live]：
 *          - 配置缺失 → 跳过（SUCCEED，不影响常规套件）
 *          - 连接失败 → FAIL（暴露错误便于诊断，如 npx 解析问题）
 *          常规套件可用 `-LE mcp_exa` 排除。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include "agent/mcp/mcp_client.h"
#include "agent/mcp/mcp_config.h"

using namespace agent;
using namespace agent::mcp;
using namespace Catch::Matchers;

namespace {

/// 用户配置目录：环境变量 WORKX_TEST_MCP_CONFIG 优先，否则平台默认 ~/.workx
std::filesystem::path user_config_dir() {
    if (const char* env = std::getenv("WORKX_TEST_MCP_CONFIG")) {
        if (env[0] != '\0') return std::filesystem::path(env);
    }
#ifdef _WIN32
    if (const char* env = std::getenv("USERPROFILE")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
#else
    if (const char* env = std::getenv("HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
#endif
    return {};
}

/// 从用户 mcp.json 中提取 "exa" server 配置；缺失返回 nullopt
std::optional<McpServerConfig> load_exa_config() {
    auto dir = user_config_dir();
    if (dir.empty()) return std::nullopt;

    auto configs = load_mcp_configs(dir, std::filesystem::current_path());
    if (configs.is_err()) return std::nullopt;

    for (const auto& cfg : configs.value()) {
        if (cfg.name == "exa") return cfg;
    }
    return std::nullopt;
}

}  // namespace

TEST_CASE("Exa MCP 实时：连接协商 + 工具清单 + 真实搜索", "[mcp_exa][live]") {
    auto cfg = load_exa_config();
    if (!cfg.has_value()) {
        SUCCEED("未找到 exa server 配置（~/.workx/mcp.json），跳过实时测试");
        return;
    }

    McpClient client;
    auto ok = client.connect(*cfg, 30000);
    if (ok.is_err()) {
        FAIL("Exa MCP 连接失败: " << ok.error().message << "（command=" << cfg->command << "）");
    }
    REQUIRE(client.is_connected());
    REQUIRE(client.name() == "exa");

    // Exa 托管端点是 1.x（2025-11-25），验证 discover → initialize 回退路径
    REQUIRE(client.protocol_version() == "2025-11-25");

    // 工具清单：web_search_exa + web_fetch_exa
    auto tools = client.list_tools();
    REQUIRE(tools.is_ok());
    REQUIRE_FALSE(tools.value().empty());
    bool has_search = false, has_fetch = false;
    for (const auto& t : tools.value()) {
        if (t.name == "web_search_exa") has_search = true;
        if (t.name == "web_fetch_exa") has_fetch = true;
    }
    REQUIRE(has_search);
    REQUIRE(has_fetch);

    // 真实搜索调用（numResults=2 控制返回量，避免超时）
    auto result =
        client.call_tool("web_search_exa", {{"query", "MCP protocol 2026"}, {"numResults", 2}});
    REQUIRE(result.is_ok());
    REQUIRE_FALSE(result.value().is_error);
    REQUIRE_FALSE(result.value().content.empty());

    // 结果应包含文本内容（标题/URL/摘要）
    bool has_text = false;
    std::string all_text;
    for (const auto& c : result.value().content) {
        if (c.type == "text") {
            has_text = true;
            all_text += c.text;
        }
    }
    REQUIRE(has_text);
    REQUIRE_THAT(all_text, ContainsSubstring("Title:"));

    client.disconnect();
    REQUIRE_FALSE(client.is_connected());
}

TEST_CASE("Exa MCP 实时：web_fetch_exa 抓取页面", "[mcp_exa][live]") {
    auto cfg = load_exa_config();
    if (!cfg.has_value()) {
        SUCCEED("未找到 exa server 配置（~/.workx/mcp.json），跳过实时测试");
        return;
    }

    McpClient client;
    auto ok = client.connect(*cfg, 30000);
    if (ok.is_err()) {
        FAIL("Exa MCP 连接失败: " << ok.error().message);
    }

    // 抓取一个已知 URL（MCP 官方博客），验证 web_fetch_exa
    auto result = client.call_tool(
        "web_fetch_exa", {{"urls", nlohmann::json::array({"https://modelcontextprotocol.io"})},
                          {"maxCharacters", 2000}});
    REQUIRE(result.is_ok());
    REQUIRE_FALSE(result.value().is_error);
    REQUIRE_FALSE(result.value().content.empty());

    bool has_text = false;
    for (const auto& c : result.value().content) {
        if (c.type == "text" && !c.text.empty()) has_text = true;
    }
    REQUIRE(has_text);

    client.disconnect();
}
