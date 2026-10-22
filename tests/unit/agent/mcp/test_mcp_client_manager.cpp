/**
 * @file test_mcp_client_manager.cpp
 * @brief McpClientManager 单元测试（Issue #27）
 * @details 覆盖空配置加载、get_client/clients/server_names/describe_servers/empty，
 *          以及端到端连接（假 stdio server）后的工具清单预取。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "agent/mcp/mcp_client_manager.h"
#include "helpers/python_command.h"

using namespace agent;
using namespace agent::mcp;
using namespace Catch::Matchers;

namespace {

std::string fake_server_path() {
    return (std::filesystem::path(SOURCE_DIR) / "tests" / "unit" / "agent" / "mcp" /
            "fake_mcp_server.py")
        .string();
}

/// 写一个仅含假 server 的 .mcp.json 到临时目录
std::filesystem::path make_project_config(const std::string& mode) {
    auto dir =
        std::filesystem::temp_directory_path() / ("workx_mcp_mgr_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir);
    // JSON 字符串中反斜杠需转义，改用正斜杠（Windows 路径兼容）
    std::string script = fake_server_path();
    std::replace(script.begin(), script.end(), '\\', '/');
    std::ofstream ofs(dir / ".mcp.json");
    ofs << R"({"mcpServers":{"fake":{"command":")" << agent::test::python_command()
        << R"(","args":[")" << script << R"("],"env":{"FAKE_MCP_MODE":")" << mode
        << R"(","PYTHONHASHSEED":"0"}}}})";
    return dir;
}

/// 后台连接是异步的：轮询等待全部 server 进入终态（已连接/失败），避免竞态
void wait_until_settled(McpClientManager& manager, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_terminal = true;
        for (const auto& st : manager.server_status()) {
            if (st.state == McpServerState::Connecting) {
                all_terminal = false;
                break;
            }
        }
        if (all_terminal) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

}  // namespace

TEST_CASE("McpClientManager 空配置加载后为空", "[mcp_manager][empty]") {
    auto dir = std::filesystem::temp_directory_path() /
               ("workx_mcp_mgr_empty_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir);

    McpClientManager manager;
    manager.load_and_connect(dir, dir);
    REQUIRE(manager.empty());
    REQUIRE(manager.server_names().empty());
    REQUIRE(manager.clients().empty());
    REQUIRE(manager.describe_servers().empty());
    REQUIRE(manager.get_client("anything") == nullptr);

    std::filesystem::remove_all(dir);
}

TEST_CASE("McpClientManager 连接假 server 后预取工具清单", "[mcp_manager][connect]") {
    auto dir = make_project_config("discover");

    McpClientManager manager;
    manager.load_and_connect(dir, dir);
    wait_until_settled(manager);

    REQUIRE_FALSE(manager.empty());
    REQUIRE(manager.server_names().size() == 1);
    REQUIRE(manager.server_names()[0] == "fake");

    auto client = manager.get_client("fake");
    REQUIRE(client != nullptr);
    REQUIRE(client->is_connected());

    auto desc = manager.describe_servers();
    REQUIRE_THAT(desc, ContainsSubstring("fake"));
    REQUIRE_THAT(desc, ContainsSubstring("echo"));
    REQUIRE_THAT(desc, ContainsSubstring("add"));

    std::filesystem::remove_all(dir);
}

TEST_CASE("McpClientManager get_client 未知名返回 nullptr", "[mcp_manager][get]") {
    auto dir = make_project_config("discover");
    McpClientManager manager;
    manager.load_and_connect(dir, dir);
    wait_until_settled(manager);

    REQUIRE(manager.get_client("ghost") == nullptr);

    std::filesystem::remove_all(dir);
}

TEST_CASE("McpClientManager server_status 返回协议与工具数", "[mcp_manager][status]") {
    auto dir = make_project_config("discover");
    McpClientManager manager;
    manager.load_and_connect(dir, dir);
    wait_until_settled(manager);

    auto status = manager.server_status();
    REQUIRE(status.size() == 1);
    REQUIRE(status[0].name == "fake");
    REQUIRE(status[0].protocol == "2026-07-28");
    REQUIRE(status[0].tool_count == 2);  // echo + add

    std::filesystem::remove_all(dir);
}

TEST_CASE("McpClientManager 连接失败记录状态与错误信息", "[mcp_manager][failed]") {
    auto dir =
        std::filesystem::temp_directory_path() / ("workx_mcp_mgr_fail_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir);
    // 命令不存在 → 连接必然失败
    std::ofstream ofs(dir / ".mcp.json");
    ofs << R"({"mcpServers":{"ghost":{"command":"no_such_cmd_xyz","args":[]}}})";
    ofs.close();  // 关闭刷新缓冲，确保 load_and_connect 读到完整配置

    McpClientManager manager;
    manager.load_and_connect(dir, dir);
    wait_until_settled(manager);

    REQUIRE(manager.empty());  // 无成功连接
    auto status = manager.server_status();
    REQUIRE(status.size() == 1);
    REQUIRE(status[0].name == "ghost");
    REQUIRE(status[0].state == McpServerState::Failed);
    REQUIRE_FALSE(status[0].error.empty());  // 失败原因已记录

    std::filesystem::remove_all(dir);
}
