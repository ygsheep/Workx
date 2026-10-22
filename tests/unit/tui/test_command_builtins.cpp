/**
 * @file test_command_builtins.cpp
 * @brief ftxtui 内置命令注册（B2 统一命令）单元测试
 * @details 验证 register_ftx_builtins 把内置命令注册进 agent 侧 CommandRegistry：
 *          命令清单完整、可执行（LocalCommand 调用）、副作用回调被正确触发。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "command/builtins.h"

using namespace ftxtui;

namespace {

/// @brief 执行注册表里的命令，返回结果文本
agent::command::CommandResult run_registered(const agent::command::CommandRegistry& reg,
                                             const std::string& name,
                                             const std::string& args = "") {
    auto cmd = reg.find_by_name(name);
    REQUIRE(cmd != nullptr);
    auto local = std::dynamic_pointer_cast<agent::command::LocalCommand>(cmd);
    REQUIRE(local != nullptr);
    agent::command::CommandContext ctx;
    return local->call(args, ctx);
}

}  // namespace

TEST_CASE("register_ftx_builtins registers the core commands", "[ftx_builtins][registry]") {
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {});
    REQUIRE(reg.size() ==
            16);  // help/exit/quit/clear/new/compact/model/provider/resume/rename/view/edit/nvim/
                  // Test:askuser/diff/rollback（新增命令须同步更新此处计数）
    REQUIRE(reg.exists("help"));
    REQUIRE(reg.exists("exit"));
    REQUIRE(reg.exists("quit"));
    REQUIRE(reg.exists("clear"));
    REQUIRE(reg.exists("new"));
    REQUIRE(reg.exists("compact"));
    REQUIRE(reg.exists("model"));
    REQUIRE(reg.exists("provider"));
    REQUIRE(reg.exists("resume"));
    REQUIRE(reg.exists("rename"));
    REQUIRE(reg.exists("view"));
    REQUIRE(reg.exists("edit"));
    REQUIRE(reg.exists("nvim"));
    REQUIRE(reg.exists("Test:askuser"));
    // #81：git 检查点只读清单 / 人工回滚入口
    REQUIRE(reg.exists("diff"));
    REQUIRE(reg.exists("rollback"));
}

TEST_CASE("register_ftx_builtins marks all as user invocable", "[ftx_builtins][registry]") {
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {});
    auto cmds = reg.get_user_invocable_commands();
    REQUIRE(cmds.size() == 16);
    for (const auto& c : cmds) {
        REQUIRE_FALSE(c->name().empty());
        REQUIRE_FALSE(c->description().empty());
    }
}

TEST_CASE("register_ftx_builtins help lists all commands", "[ftx_builtins][exec]") {
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {});
    auto res = run_registered(reg, "help");
    REQUIRE(res.type == agent::command::CommandResult::Type::Text);
    REQUIRE_FALSE(res.is_error);
    REQUIRE(res.text.find("/model") != std::string::npos);
    REQUIRE(res.text.find("/resume") != std::string::npos);
}

TEST_CASE("register_ftx_builtins exit triggers on_exit callback", "[ftx_builtins][exec]") {
    int exited = 0;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_exit = [&] { ++exited; }});
    run_registered(reg, "exit");
    REQUIRE(exited == 1);
}

TEST_CASE("register_ftx_builtins model triggers on_model_select callback", "[ftx_builtins][exec]") {
    int opened = 0;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_model_select = [&] { ++opened; }});
    run_registered(reg, "model");
    REQUIRE(opened == 1);
}

TEST_CASE("register_ftx_builtins resume passes args to callback", "[ftx_builtins][exec]") {
    std::string captured;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_resume = [&](const std::string& args) { captured = args; }});
    run_registered(reg, "resume", "2");
    REQUIRE(captured == "2");
}

TEST_CASE("register_ftx_builtins rename passes title to callback", "[ftx_builtins][exec]") {
    std::string captured;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_rename = [&](const std::string& args) { captured = args; }});
    run_registered(reg, "rename", "新标题");
    REQUIRE(captured == "新标题");
}

TEST_CASE("register_ftx_builtins clear triggers on_clear callback", "[ftx_builtins][exec]") {
    int cleared = 0;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_clear = [&] { ++cleared; }});
    run_registered(reg, "clear");
    REQUIRE(cleared == 1);
}

TEST_CASE("register_ftx_builtins new triggers on_new callback", "[ftx_builtins][exec]") {
    int created = 0;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_new = [&] { ++created; }});
    run_registered(reg, "new");
    REQUIRE(created == 1);
}

TEST_CASE("register_ftx_builtins view passes path to callback", "[ftx_builtins][exec]") {
    std::string captured;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_view = [&](const std::string& args) { captured = args; }});
    run_registered(reg, "view", "src/main.cpp");
    REQUIRE(captured == "src/main.cpp");
}

TEST_CASE("register_ftx_builtins edit passes path to callback", "[ftx_builtins][exec]") {
    std::string captured;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_edit = [&](const std::string& args) { captured = args; }});
    run_registered(reg, "edit", "src/main.cpp");
    REQUIRE(captured == "src/main.cpp");
}

TEST_CASE("register_ftx_builtins Test:askuser triggers on_test_askuser callback",
          "[ftx_builtins][exec]") {
    int opened = 0;
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {.on_test_askuser = [&] { ++opened; }});
    run_registered(reg, "Test:askuser");
    REQUIRE(opened == 1);
}

TEST_CASE("register_ftx_builtins without callbacks is safe (no throw)", "[ftx_builtins][exec]") {
    agent::command::CommandRegistry reg;
    register_ftx_builtins(reg, {});
    REQUIRE_NOTHROW(run_registered(reg, "exit"));
    REQUIRE_NOTHROW(run_registered(reg, "clear"));
    REQUIRE_NOTHROW(run_registered(reg, "new"));
    REQUIRE_NOTHROW(run_registered(reg, "model"));
    REQUIRE_NOTHROW(run_registered(reg, "resume", "1"));
    REQUIRE_NOTHROW(run_registered(reg, "rename", "t"));
    REQUIRE_NOTHROW(run_registered(reg, "view", "file.txt"));
    REQUIRE_NOTHROW(run_registered(reg, "edit", "file.txt"));
    REQUIRE_NOTHROW(run_registered(reg, "Test:askuser"));
}