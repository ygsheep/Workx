/**
 * @file test_skill_tool.cpp
 * @brief SkillTool 单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/tool/SkillTool/skill_tool.h"
#include "agent/command/inclaude/command.h"
#include "core/utils/error.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::command;

namespace {

std::shared_ptr<PromptCommand> make_skill_cmd(const std::string& name, const std::string& body) {
    auto cmd = std::make_shared<PromptCommand>(name, "desc of " + name);
    cmd->set_prompt_generator([body](const std::string& /*args*/, const CommandContext& /*ctx*/) {
        PromptBlock block;
        block.type = PromptBlockType::Text;
        block.text = "Base directory for this skill: /x\n\n" + body;
        return std::vector<PromptBlock>{std::move(block)};
    });
    cmd->set_loaded_from(LoadSource::Skills);
    return cmd;
}

/// @brief 填充工具上下文（ToolContext 不可拷贝，按左值传入）
void fill_ctx(ToolContext& ctx) { ctx.cwd = "C:\\proj"; }

}  // anonymous namespace

TEST_CASE("SkillTool returns skill body by name", "[skill][tool]") {
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("build", "Run the build:\n1. configure\n2. compile"));

    ToolContext ctx;
    fill_ctx(ctx);
    SkillTool tool(registry);
    const auto result = tool.call({{"name", "build"}}, ctx);

    REQUIRE(result.is_ok());
    const auto& text = result.value().text;
    REQUIRE(text.find("Run the build:") != std::string::npos);
    REQUIRE(text.find("2. compile") != std::string::npos);
}

TEST_CASE("SkillTool returns not found error", "[skill][tool]") {
    auto registry = std::make_shared<CommandRegistry>();
    ToolContext ctx;
    fill_ctx(ctx);
    SkillTool tool(registry);

    const auto result = tool.call({{"name", "missing"}}, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ResourceNotFound);
}

TEST_CASE("SkillTool rejects missing or empty name", "[skill][tool]") {
    auto registry = std::make_shared<CommandRegistry>();
    ToolContext ctx;
    fill_ctx(ctx);
    SkillTool tool(registry);

    REQUIRE(tool.call({{}}, ctx).is_err());
    REQUIRE(tool.call({{"name", ""}}, ctx).is_err());
    const auto result = tool.call({{"name", ""}}, ctx);
    REQUIRE(result.error().code == Error::Code::InvalidInput);
}

TEST_CASE("SkillTool rejects non-prompt commands", "[skill][tool]") {
    auto registry = std::make_shared<CommandRegistry>();
    auto local = std::make_shared<LocalCommand>("echo", "local");
    local->set_call(
        [](const std::string&, const CommandContext&) { return CommandResult::ok("hi"); });
    registry->register_command(local);

    ToolContext ctx;
    fill_ctx(ctx);
    SkillTool tool(registry);
    const auto result = tool.call({{"name", "echo"}}, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::InvalidInput);
}
