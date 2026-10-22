/**
 * @file test_agent_skills_mcp.cpp
 * @brief #56 方案 C（技能预载）与方案 D（MCP 生命周期）集成测试
 * @details 与 tests/unit/agent/tool/test_task_tools.cpp 的纯函数单测互补：
 *   - 方案 C：走完整 AgentTool.call() → 子 Agent ReActLoop → provider 请求链路，
 *             验证 skills 预载全文真实进入子 Agent 初始 system 消息（不依赖外部 LLM）。
 *   - 方案 D：基于真实 fake_mcp_server.py 进程，验证 inline 连接/工具预取、
 *             字符串引用复用、以及收尾 dispose 清理的生命周期闭环。
 *
 * 依赖：本机 Python 可运行 fake_mcp_server.py（与单元测试相同的前提）。
 *       解释器名走 agent::test::python_command()（Windows "python" / 其他 "python3"）。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "agent/tool/AgentTool/agent_tool.h"
#include "agent/tool/MCPTool/mcp_tool.h"
#include "agent/tool/registry.h"
#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/registry.h"
#include "agent/mcp/mcp_client_manager.h"
#include "core/task/task_manager.h"
#include "../unit/helpers/python_command.h"
#include "core/events/event_bus.h"

#include "helpers/mock_config_manager.h"
#include "helpers/mock_event_bus.h"
#include "helpers/mock_provider.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::test;
using namespace Catch::Matchers;

namespace {

/// @brief 定位 fake MCP server 脚本（tests/unit/agent/mcp/fake_mcp_server.py）
std::string fake_mcp_server_path() {
    return (std::filesystem::path(SOURCE_DIR) / "tests" / "unit" / "agent" / "mcp" /
            "fake_mcp_server.py")
        .string();
}

/// @brief 构造指向 fake MCP server 的 inline server 对象（方案 D 的 mcpServers 元素）
nlohmann::json inline_fake_server(const std::string& mode = "discover") {
    std::string script = fake_mcp_server_path();
    std::replace(script.begin(), script.end(), '\\', '/');
    return {
        {"name", "fake"},
        {"command", agent::test::python_command()},
        {"args", nlohmann::json::array({script})},
        {"env", {{"FAKE_MCP_MODE", mode}, {"PYTHONHASHSEED", "0"}}},
    };
}

/// @brief 写一个仅含 fake server 的 .mcp.json 到临时目录（供父 manager load_and_connect）
std::filesystem::path make_project_config_with_fake(const std::string& mode) {
    auto dir =
        std::filesystem::temp_directory_path() / ("workx_mcp_it_" + std::to_string(::rand()));
    std::filesystem::create_directories(dir);
    std::string script = fake_mcp_server_path();
    std::replace(script.begin(), script.end(), '\\', '/');
    std::ofstream ofs(dir / ".mcp.json");
    ofs << R"({"mcpServers":{"fake":{"command":")" << agent::test::python_command()
        << R"(","args":[")" << script << R"("],"env":{"FAKE_MCP_MODE":")" << mode
        << R"(","PYTHONHASHSEED":"0"}}}})";
    ofs.close();
    return dir;
}

/// @brief 后台连接异步：轮询等待全部 server 进入终态，避免竞态
void wait_until_settled(mcp::McpClientManager& manager, int timeout_ms = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        bool all_terminal = true;
        for (const auto& st : manager.server_status()) {
            if (st.state == mcp::McpServerState::Connecting) {
                all_terminal = false;
                break;
            }
        }
        if (all_terminal) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/// @brief 集成测试夹具：清理单例残留（与 test_task_tools 一致）
struct AgentToolsFixture {
    AgentToolsFixture() {
        EventBus::instance().clear();
        TaskManager::instance().cancelAll();
        TaskManager::instance().waitForAll();
        TaskManager::instance().update();
    }
    ~AgentToolsFixture() {
        TaskManager::instance().cancelAll();
        TaskManager::instance().waitForAll();
        TaskManager::instance().update();
        EventBus::instance().clear();
    }
};

/// @brief 构造来源为 Skills 的 PromptCommand（模拟从磁盘加载的 skill）
std::shared_ptr<command::PromptCommand> make_skill_cmd(
    const std::string& name, const std::vector<std::string>& text_blocks) {
    auto cmd = command::make_prompt_command(name, "skill description");
    cmd->set_loaded_from(command::LoadSource::Skills);
    cmd->set_prompt_generator([text_blocks](const std::string&, const command::CommandContext&) {
        std::vector<command::PromptBlock> blocks;
        for (const auto& t : text_blocks) {
            blocks.push_back({command::PromptBlockType::Text, t});
        }
        return blocks;
    });
    return cmd;
}

/// @brief 在消息列表中查找首条以 prefix 开头的 System 消息，命中时把全文写入 out
bool find_system_msg_prefix(const std::vector<ChatMessage>& msgs, const std::string& prefix,
                            std::string& out) {
    for (const auto& m : msgs) {
        if (m.role == ChatMessage::Role::System && m.content.rfind(prefix, 0) == 0) {
            out = m.content;
            return true;
        }
    }
    return false;
}

}  // namespace

// ============================================================
// #56 方案 C：技能预载集成
// ============================================================

TEST_CASE_METHOD(AgentToolsFixture, "方案C 集成：AgentTool skills 预载全文进入子 Agent 初始请求",
                 "[agent_tool][skill_preload][integration]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;

    // 真实 CommandRegistry + 一个来源为 Skills 的 skill
    command::CommandRegistry cmd_registry;
    cmd_registry.register_command(
        make_skill_cmd("review", {"你是资深 C++ 评审员。", "关注正确性与回归风险。"}));

    // 捕获 provider：子 Agent 仅一轮请求即给出最终答复
    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("reviewed");
    provider->set_next_reader(reader);

    ToolContext ctx;
    ctx.cwd = ".";
    ctx.session_id = "plan-c-integration";
    ctx.task_manager_ptr = &tm;
    ctx.config_manager_ptr = &cfg;
    ctx.event_bus_ptr = &bus;
    ctx.provider_ptr = provider.get();
    ctx.command_registry_ptr = &cmd_registry;  // #56 方案 C：技能全文来源

    AgentTool tool;
    auto r = tool.call(
        nlohmann::json{
            {"prompt", "review the code"},
            {"skills", nlohmann::json::array({"review"})},
        },
        ctx);
    REQUIRE(r.is_ok());

    // 子 Agent 在后台线程池执行并完成
    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());
    REQUIRE(tasks[0]->output().find("reviewed") != std::string::npos);

    // 关键断言：子 Agent 发送给 provider 的请求消息含预载的 skill 全文 system 消息
    // （验证 ctx.command_registry_ptr → SubAgentLaunchOptions.skills →
    //   build_skill_preload_messages → 初始 messages 的完整接线）
    std::string system_msg;
    REQUIRE(find_system_msg_prefix(provider->last_messages, "Skill: review", system_msg));
    REQUIRE(system_msg.find("你是资深 C++ 评审员。") != std::string::npos);
    REQUIRE(system_msg.find("关注正确性与回归风险。") != std::string::npos);
}

TEST_CASE_METHOD(AgentToolsFixture, "方案C 集成：未知名 skill 不阻断子 Agent 启动",
                 "[agent_tool][skill_preload][integration]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;

    command::CommandRegistry cmd_registry;  // 空 registry

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("ok without skill");
    provider->set_next_reader(reader);

    ToolContext ctx;
    ctx.cwd = ".";
    ctx.session_id = "plan-c-unknown-skill";
    ctx.task_manager_ptr = &tm;
    ctx.config_manager_ptr = &cfg;
    ctx.event_bus_ptr = &bus;
    ctx.provider_ptr = provider.get();
    ctx.command_registry_ptr = &cmd_registry;

    AgentTool tool;
    auto r = tool.call(
        nlohmann::json{
            {"prompt", "do the task"},
            {"skills", nlohmann::json::array({"no_such_skill"})},
        },
        ctx);
    REQUIRE(r.is_ok());

    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());
    REQUIRE(tasks[0]->output().find("ok without skill") != std::string::npos);

    // 未知名 skill 静默跳过：请求消息中无 "Skill: no_such_skill" system 消息
    std::string system_msg;
    REQUIRE_FALSE(
        find_system_msg_prefix(provider->last_messages, "Skill: no_such_skill", system_msg));
}

// ============================================================
// #56 方案 D：MCP 生命周期集成
// ============================================================

TEST_CASE("方案D 集成：inline mcpServers 连接 fake server 并预取工具",
          "[agent_tool][mcp_scope][integration]") {
    nlohmann::json servers = nlohmann::json::array({inline_fake_server("discover")});
    auto r = AgentTool::build_mcp_scope(servers, nullptr);

    // 作用域构建成功且可用（非空）
    REQUIRE(r.scope != nullptr);
    REQUIRE_FALSE(r.scope->empty());
    REQUIRE(r.scope->server_names().size() == 1);
    REQUIRE(r.scope->server_names()[0] == "fake");

    // 工具清单已预取（fake server 暴露 echo + add）
    const std::string desc = r.scope->describe_servers();
    REQUIRE_THAT(desc, ContainsSubstring("echo"));
    REQUIRE_THAT(desc, ContainsSubstring("add"));

    // inline 连接为独立 client，进入 owned_clients（子 Agent 收尾需 dispose）
    REQUIRE(r.owned_clients.size() == 1);
    REQUIRE(r.owned_clients[0]->is_connected());

    // 收尾清理路径：dispose 后 client 断开（方案 D 生命周期闭环）
    r.scope->dispose(r.owned_clients[0]);
    REQUIRE_FALSE(r.owned_clients[0]->is_connected());
}

TEST_CASE("方案D 集成：inline 连接失败静默跳过，不抛异常", "[agent_tool][mcp_scope][integration]") {
    // 命令不存在 → connect_one_off 返回 nullptr，build_mcp_scope 静默跳过
    nlohmann::json servers = nlohmann::json::array({
        {{"name", "ghost"}, {"command", "no_such_cmd_xyz"}, {"args", nlohmann::json::array()}},
    });
    auto r = AgentTool::build_mcp_scope(servers, nullptr);
    REQUIRE(r.scope != nullptr);
    REQUIRE(r.scope->empty());  // 无成功连接
    REQUIRE(r.owned_clients.empty());
}

TEST_CASE_METHOD(AgentToolsFixture, "方案D 集成：字符串引用复用父 client 且不产生 owned_clients",
                 "[agent_tool][mcp_scope][integration]") {
    // 父全局管理器先连接 fake server
    mcp::McpClientManager parent(nullptr);
    auto dir = make_project_config_with_fake("discover");
    parent.load_and_connect(dir, dir);
    wait_until_settled(parent);
    REQUIRE_FALSE(parent.empty());
    auto parent_client = parent.get_client("fake");
    REQUIRE(parent_client != nullptr);

    // 字符串引用：复用父 client，不新建连接、不进入 owned_clients（不 dispose）
    nlohmann::json servers = nlohmann::json::array({"fake"});
    auto r = AgentTool::build_mcp_scope(servers, &parent);
    REQUIRE(r.scope != nullptr);
    REQUIRE_FALSE(r.scope->empty());
    REQUIRE(r.scope->server_names().size() == 1);
    REQUIRE(r.owned_clients.empty());

    // 同一 client 实例（复用父 memoized client，而非新建）
    REQUIRE(r.scope->get_client("fake") == parent_client);

    std::filesystem::remove_all(dir);
}

TEST_CASE_METHOD(AgentToolsFixture, "方案D 集成：子 Agent 经 inline mcpServers 端到端调用 MCP 工具",
                 "[agent_tool][mcp_scope][integration]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;

    // 父全局 MCP 管理器（空；仅作 MCPTool 构造绑定的 fallback）
    auto parent_manager = std::make_shared<mcp::McpClientManager>(nullptr);

    // 父 registry 注册 MCPTool（子 Agent 独立工具集从父 registry 裁剪而来）
    auto registry = std::make_shared<ToolRegistry>();
    registry->register_tool(std::make_shared<MCPTool>(parent_manager));

    // 捕获 provider：第一轮返回 MCP echo 工具调用，第二轮返回最终答复
    auto provider = std::make_shared<MockCompletionProvider>();
    auto r1 = std::make_shared<MockStreamReader>();
    r1->add_tool_use_start("mcp_1", "MCP");
    r1->add_tool_use_delta("mcp_1", R"({"server":"fake","tool":"echo","input":{"text":"hi"}})");
    auto r2 = std::make_shared<MockStreamReader>();
    r2->add_content_chunk("mcp done");
    provider->set_next_reader(r1);
    provider->set_next_reader(r2);

    ToolContext ctx;
    ctx.cwd = ".";
    ctx.session_id = "plan-d-integration";
    ctx.task_manager_ptr = &tm;
    ctx.config_manager_ptr = &cfg;
    ctx.event_bus_ptr = &bus;
    ctx.provider_ptr = provider.get();
    ctx.tool_registry = registry;
    ctx.permission_mode = PermissionMode::BypassPermissions;  // 跳过 MCPTool AskUser 确认

    AgentTool tool;
    auto r = tool.call(
        nlohmann::json{
            {"prompt", "call the MCP echo tool"},
            {"mcpServers", nlohmann::json::array({inline_fake_server("discover")})},
        },
        ctx);
    REQUIRE(r.is_ok());

    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());
    REQUIRE(tasks[0]->output().find("mcp done") != std::string::npos);

    // 子 Agent 工具 schema 含 MCP 工具（MCPTool 进入独立子 registry 并下发）
    REQUIRE(provider->last_tools.is_array());
    bool has_mcp_schema = false;
    for (const auto& s : provider->last_tools) {
        if (s.value("name", "") == "MCP") has_mcp_schema = true;
    }
    REQUIRE(has_mcp_schema);

    // 第二轮请求的消息含工具观察结果：证明子 Agent 经作用域 manager 真实调用了
    // inline fake server（ctx.mcp_manager_ptr 优先于 MCPTool 构造期父 manager）
    bool saw_echo = false;
    for (const auto& m : provider->last_messages) {
        if (m.role == ChatMessage::Role::Tool && m.content.find("echo: hi") != std::string::npos) {
            saw_echo = true;
        }
    }
    REQUIRE(saw_echo);
}
