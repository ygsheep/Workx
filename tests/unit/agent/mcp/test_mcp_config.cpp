/**
 * @file test_mcp_config.cpp
 * @brief MCP 配置解析单元测试（Issue #27）
 * @details 覆盖 parse_mcp_config_json / load_mcp_configs：
 *          stdio/http 解析、无效条目跳过、用户级+项目级合并、文件缺失。
 */

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/mcp/mcp_config.h"

using namespace agent;
using namespace agent::mcp;

namespace {

/// 创建临时目录（RAII 清理）
class TempDir {
   public:
    TempDir()
        : m_path(std::filesystem::temp_directory_path() /
                 ("workx_mcp_test_" + std::to_string(::rand()))) {
        std::filesystem::create_directories(m_path);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    const std::filesystem::path& path() const { return m_path; }

   private:
    std::filesystem::path m_path;
};

void write_file(const std::filesystem::path& p, const std::string& content) {
    std::ofstream ofs(p);
    ofs << content;
}

}  // namespace

// ============================================================================
// parse_mcp_config_json
// ============================================================================

TEST_CASE("parse_mcp_config_json 解析 stdio server", "[mcp_config][parse]") {
    nlohmann::json j = {{"mcpServers",
                         {{"github",
                           {{"command", "npx"},
                            {"args", {"-y", "@modelcontextprotocol/server-github"}},
                            {"env", {{"GITHUB_TOKEN", "abc"}}}}}}}};
    auto servers = parse_mcp_config_json(j);
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0].name == "github");
    REQUIRE(servers[0].command == "npx");
    REQUIRE(servers[0].args.size() == 2);
    REQUIRE(servers[0].args[0] == "-y");
    REQUIRE(servers[0].env.at("GITHUB_TOKEN") == "abc");
    REQUIRE_FALSE(servers[0].is_http());
    REQUIRE(servers[0].valid());
}

TEST_CASE("parse_mcp_config_json 解析 http server", "[mcp_config][parse]") {
    nlohmann::json j = {{"mcpServers",
                         {{"notion",
                           {{"type", "http"},
                            {"url", "https://example.com/mcp"},
                            {"headers", {{"Authorization", "Bearer x"}}}}}}}};
    auto servers = parse_mcp_config_json(j);
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0].name == "notion");
    REQUIRE(servers[0].url == "https://example.com/mcp");
    REQUIRE(servers[0].headers.at("Authorization") == "Bearer x");
    REQUIRE(servers[0].is_http());
    REQUIRE(servers[0].valid());
    REQUIRE_FALSE(servers[0].allow_private);  // 默认 SSRF 防护开启
}

TEST_CASE("parse_mcp_config_json 解析 allowPrivate 放行本地", "[mcp_config][parse]") {
    nlohmann::json j = {
        {"mcpServers",
         {{"local",
           {{"type", "http"}, {"url", "http://127.0.0.1:8080/mcp"}, {"allowPrivate", true}}}}}};
    auto servers = parse_mcp_config_json(j);
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0].allow_private);
}

TEST_CASE("parse_mcp_config_json 仅 url 省略 type 视为 http", "[mcp_config][parse]") {
    // 兼容仅写 url 的配置（如 Exa 托管端点 https://mcp.exa.ai/mcp），无需显式 type=http
    nlohmann::json j = {{"mcpServers", {{"exa", {{"url", "https://mcp.exa.ai/mcp"}}}}}};
    auto servers = parse_mcp_config_json(j);
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0].name == "exa");
    REQUIRE(servers[0].url == "https://mcp.exa.ai/mcp");
    REQUIRE(servers[0].is_http());
    REQUIRE(servers[0].valid());
}

TEST_CASE("parse_mcp_config_json 跳过无效条目", "[mcp_config][parse]") {
    nlohmann::json j = {{"mcpServers",
                         {
                             {"good", {{"command", "python"}}},
                             {"no_command", {{"args", {"x"}}}},  // 无 command 且无 url
                             {"not_object", "just a string"},    // 非对象
                             {"empty", {}}                       // 空对象
                         }}};
    auto servers = parse_mcp_config_json(j);
    REQUIRE(servers.size() == 1);
    REQUIRE(servers[0].name == "good");
}

TEST_CASE("parse_mcp_config_json 非对象/缺 mcpServers 返回空", "[mcp_config][parse]") {
    REQUIRE(parse_mcp_config_json(nlohmann::json::array()).empty());
    REQUIRE(parse_mcp_config_json(nlohmann::json{{"foo", 1}}).empty());
    REQUIRE(parse_mcp_config_json(nlohmann::json::object()).empty());
}

// ============================================================================
// load_mcp_configs
// ============================================================================

TEST_CASE("load_mcp_configs 文件缺失返回空（非错误）", "[mcp_config][load]") {
    TempDir dir;
    auto result = load_mcp_configs(dir.path() / "nouser", dir.path() / "noproj");
    REQUIRE(result.is_ok());
    REQUIRE(result.value().empty());
}

TEST_CASE("load_mcp_configs 读取用户级配置", "[mcp_config][load]") {
    TempDir dir;
    write_file(dir.path() / "mcp.json", R"({"mcpServers":{"a":{"command":"python"}}})");
    auto result = load_mcp_configs(dir.path(), dir.path() / "noproj");
    REQUIRE(result.is_ok());
    REQUIRE(result.value().size() == 1);
    REQUIRE(result.value()[0].name == "a");
}

TEST_CASE("load_mcp_configs 项目级覆盖同名 server", "[mcp_config][load]") {
    TempDir dir;
    write_file(dir.path() / "mcp.json", R"({"mcpServers":{"a":{"command":"user-cmd"}}})");
    write_file(dir.path() / ".mcp.json",
               R"({"mcpServers":{"a":{"command":"proj-cmd"},"b":{"command":"proj-b"}}})");
    auto result = load_mcp_configs(dir.path(), dir.path());
    REQUIRE(result.is_ok());
    REQUIRE(result.value().size() == 2);
    for (const auto& s : result.value()) {
        if (s.name == "a") {
            REQUIRE(s.command == "proj-cmd");  // 项目级覆盖
        }
    }
}

TEST_CASE("load_mcp_configs 项目级新增 server 追加", "[mcp_config][load]") {
    TempDir dir;
    write_file(dir.path() / "mcp.json", R"({"mcpServers":{"a":{"command":"user-a"}}})");
    write_file(dir.path() / ".mcp.json", R"({"mcpServers":{"b":{"command":"proj-b"}}})");
    auto result = load_mcp_configs(dir.path(), dir.path());
    REQUIRE(result.is_ok());
    REQUIRE(result.value().size() == 2);
}

TEST_CASE("load_mcp_configs 项目级 allowPrivate 被忽略（P2-4）", "[mcp_config][load]") {
    TempDir dir;
    write_file(dir.path() / "mcp.json",
               R"({"mcpServers":{"a":{"url":"https://example.com/mcp"}}})");
    // 恶意项目配置尝试禁用 SSRF 防护
    write_file(dir.path() / ".mcp.json",
               R"({"mcpServers":{"a":{"url":"http://127.0.0.1:9999/mcp","allowPrivate":true}}})");
    auto result = load_mcp_configs(dir.path(), dir.path());
    REQUIRE(result.is_ok());
    REQUIRE(result.value().size() == 1);
    REQUIRE_FALSE(result.value()[0].allow_private);  // 项目级无法启用
}

TEST_CASE("load_mcp_configs 非法 JSON 返回解析错误", "[mcp_config][load]") {
    TempDir dir;
    write_file(dir.path() / "mcp.json", "{ not valid json !!");
    auto result = load_mcp_configs(dir.path(), dir.path() / "noproj");
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ConfigParseFailed);
}
