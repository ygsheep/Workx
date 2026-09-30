/**
 * @file test_view_model_mcp.cpp
 * @brief #27 M4：ActionMcpStatus 更新 SidebarModel.mcp_servers（结构化状态 + 错误）
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "bridge/action.h"
#include "vm/view_model.h"

using namespace ftxtui;

TEST_CASE("ViewModel ActionMcpStatus stores structured server entries", "[view_model][mcp]") {
    ViewModel vm;
    REQUIRE(vm.sidebar.mcp_servers.empty());

    std::vector<McpServerLite> servers = {
        {"github", "2026-07-28", 3, 1, ""},    // 已连接
        {"notion", "2025-11-25", 0, 0, ""},    // 连接中
        {"broken", "", 0, 2, "spawn failed"},  // 失败 + 错误
    };
    REQUIRE(vm.apply(ActionMcpStatus{.servers = servers}));
    REQUIRE(vm.sidebar.mcp_servers.size() == 3);

    const auto& g = vm.sidebar.mcp_servers[0];
    REQUIRE(g.name == "github");
    REQUIRE(g.protocol == "2026-07-28");
    REQUIRE(g.tool_count == 3);
    REQUIRE(g.state == 1);
    REQUIRE(g.error.empty());

    const auto& n = vm.sidebar.mcp_servers[1];
    REQUIRE(n.name == "notion");
    REQUIRE(n.state == 0);

    const auto& b = vm.sidebar.mcp_servers[2];
    REQUIRE(b.name == "broken");
    REQUIRE(b.state == 2);
    REQUIRE(b.error == "spawn failed");
}

TEST_CASE("ViewModel ActionMcpStatus identical snapshot returns false", "[view_model][mcp]") {
    ViewModel vm;
    std::vector<McpServerLite> servers = {{"github", "2026-07-28", 2, 1, ""}};
    REQUIRE(vm.apply(ActionMcpStatus{.servers = servers}));
    // 相同快照 → 无变化，不触发重绘
    REQUIRE_FALSE(vm.apply(ActionMcpStatus{.servers = servers}));
}

TEST_CASE("ViewModel ActionMcpStatus state change triggers redraw", "[view_model][mcp]") {
    ViewModel vm;
    std::vector<McpServerLite> connecting = {{"github", "", 0, 0, ""}};
    REQUIRE(vm.apply(ActionMcpStatus{.servers = connecting}));
    // 连接中 → 失败（状态变化 → 触发重绘）
    std::vector<McpServerLite> failed = {{"github", "", 0, 2, "timeout"}};
    REQUIRE(vm.apply(ActionMcpStatus{.servers = failed}));
    REQUIRE(vm.sidebar.mcp_servers[0].state == 2);
    REQUIRE(vm.sidebar.mcp_servers[0].error == "timeout");
}

TEST_CASE("ViewModel ActionMcpStatus empty snapshot clears servers", "[view_model][mcp]") {
    ViewModel vm;
    std::vector<McpServerLite> servers = {{"github", "2026-07-28", 2, 1, ""}};
    vm.apply(ActionMcpStatus{.servers = servers});
    REQUIRE(vm.apply(ActionMcpStatus{.servers = {}}));
    REQUIRE(vm.sidebar.mcp_servers.empty());
}
