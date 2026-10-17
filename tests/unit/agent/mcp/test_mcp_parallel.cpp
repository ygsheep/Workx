#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "agent/mcp/mcp_client_manager.h"
#include "agent/mcp/mcp_stdio_process.h"
#include "agent/tool/MCPTool/mcp_tool.h"
#include "agent/tool/executor.h"

using namespace agent;
using namespace agent::mcp;
using namespace agent::tool;
using namespace Catch::Matchers;

namespace {

class ParallelFixture {
  public:
    ParallelFixture() {
        dir =
            std::filesystem::temp_directory_path() /
            ("workx_parallel_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(dir);
        log = dir / "requests.jsonl";
        const auto script =
            std::filesystem::path(SOURCE_DIR) / "tests/unit/agent/mcp/fake_parallel_mcp_server.py";
        REQUIRE(server.start("python", {script.string()}, {{"PARALLEL_TEST_LOG", log.string()}}).is_ok());
        auto line = server.read_line(10000);
        REQUIRE(line.is_ok());
        REQUIRE_THAT(line.value(), StartsWith("PORT="));
        url = "http://127.0.0.1:" + line.value().substr(5) + "/mcp";
        std::ofstream(dir / "mcp.json") << nlohmann::json(
            {{"mcpServers",
              {{"parallel", {{"preset", "parallel-search"}, {"url", url}, {"allowPrivate", true}}}}}});
    }
    ~ParallelFixture() {
        server.stop();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    std::vector<nlohmann::json> requests() const {
        std::ifstream in(log);
        std::vector<nlohmann::json> rows;
        std::string line;
        while (std::getline(in, line))
            rows.push_back(nlohmann::json::parse(line));
        return rows;
    }
    std::filesystem::path dir, log;
    std::string url;
    McpStdioProcess server;
};

void wait_connected(const std::shared_ptr<McpClientManager>& manager) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!manager->get_client("parallel") && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(manager->get_client("parallel") != nullptr);
}

nlohmann::json search_input(const std::string& objective = "Find CMake documentation") {
    return {{"server", "parallel"},
            {"tool", "web_search"},
            {"input",
             {{"objective", objective}, {"search_queries", {"CMake FetchContent official documentation"}}}}};
}

} // namespace

TEST_CASE("Parallel 预设经配置加载、管理器和工具分发执行搜索与抓取", "[mcp_parallel]") {
    ParallelFixture fixture;
    auto manager = std::make_shared<McpClientManager>();
    manager->load_and_connect(fixture.dir, fixture.dir / "project");
    wait_connected(manager);
    auto registry = std::make_shared<ToolRegistry>();
    auto tool = std::make_shared<MCPTool>(manager);
    registry->register_tool(tool);
    ToolExecutor executor(registry);
    ToolContext ctx;
    ctx.permission_mode = PermissionMode::BypassPermissions;
    REQUIRE_THAT(tool->prompt(), ContainsSubstring("web_search"));
    auto result = executor.execute("MCP", search_input(), ctx);
    REQUIRE(result.is_ok());
    REQUIRE_THAT(result.value().to_string(), ContainsSubstring("https://cmake.org/"));
    auto fetched = executor.execute(
        "MCP",
        {{"server", "parallel"},
         {"tool", "web_fetch"},
         {"input", {{"urls", {"https://cmake.org/cmake/help/latest/module/FetchContent.html"}}}}},
        ctx);
    REQUIRE(fetched.is_ok());
    REQUIRE_THAT(fetched.value().to_string(), ContainsSubstring("Populate content at configure time"));

    auto rows = fixture.requests();
    REQUIRE(rows.size() >= 5);
    bool search = false, fetch = false, initialize = false, list = false;
    for (const auto& row : rows) {
        REQUIRE(row.at("path") == "/mcp");
        REQUIRE(row.at("ua") == "Workx (+https://github.com/ygsheep/Workx)");
        REQUIRE(row.at("authorization").is_null());
        const auto& request = row.at("request");
        initialize |= request.at("method") == "initialize";
        list |= request.at("method") == "tools/list";
        if (request.at("method") == "tools/call") {
            const auto& params = request.at("params");
            if (params.at("name") == "web_search") {
                search = true;
                REQUIRE(params.at("arguments").at("search_queries")[0] ==
                        "CMake FetchContent official documentation");
            } else if (params.at("name") == "web_fetch") {
                fetch = true;
                REQUIRE(params.at("arguments").at("urls")[0] ==
                        "https://cmake.org/cmake/help/latest/module/FetchContent.html");
            }
        }
    }
    REQUIRE(initialize);
    REQUIRE(list);
    REQUIRE(search);
    REQUIRE(fetch);

    const auto count = rows.size();
    ctx.permission_mode = PermissionMode::Default; // 无确认通道时 fail closed
    auto denied = executor.execute("MCP", search_input(), ctx);
    REQUIRE(denied.is_err());
    REQUIRE(denied.error().code == Error::Code::PermissionDenied);
    ctx.permission_mode = PermissionMode::BypassPermissions;
    ctx.session_mode = SessionMode::Minimal;
    REQUIRE(executor.execute("MCP", search_input(), ctx).is_err());
    ctx.session_mode = SessionMode::Standard;
    std::atomic<bool> cancelled{true};
    ctx.cancel_flag = &cancelled;
    auto stopped = executor.execute("MCP", search_input(), ctx);
    REQUIRE(stopped.is_err());
    REQUIRE(stopped.error().code == Error::Code::Cancelled);
    REQUIRE(fixture.requests().size() == count);

    ctx.cancel_flag = nullptr;
    auto rpc_error = executor.execute("MCP", search_input("rpc-error"), ctx);
    REQUIRE(rpc_error.is_err());
    REQUIRE_THAT(rpc_error.error().message, ContainsSubstring("Invalid search arguments"));
    auto tool_error = executor.execute("MCP", search_input("tool-error"), ctx);
    REQUIRE(tool_error.is_ok()); // 现有 MCPTool 将 isError 显示为模型可读的错误文本
    REQUIRE_THAT(tool_error.value().to_string(), ContainsSubstring("Search quota exceeded"));
}

TEST_CASE("Parallel 预设沿用 HTTP 请求超时", "[mcp_parallel]") {
    ParallelFixture fixture;
    auto loaded = load_mcp_configs(fixture.dir, fixture.dir / "project");
    REQUIRE(loaded.is_ok());
    HttpMcpTransport transport(loaded.value()[0]);
    REQUIRE(transport.start().is_ok());
    auto result =
        transport.send_request({{"jsonrpc", "2.0"},
                                {"id", 1},
                                {"method", "tools/call"},
                                {"params", {{"name", "web_search"}, {"arguments", {{"objective", "slow"}}}}}},
                               50);
    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::NetworkTimeout);
}
