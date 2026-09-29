/**
 * @file skill_tool.cpp
 * @brief SkillTool 实现
 * @details 通过 CommandRegistry 按名称查找 PromptCommand，展开其提示词。
 *          未找到时返回 ResourceNotFound 错误。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/SkillTool/skill_tool.h"

#include "agent/command/inclaude/command.h"
#include "agent/skill/inclaude/skill_prompt.h"  // build_skill_full_text 共享取全文 helper
#include "core/utils/error.h"

namespace agent::tool {

namespace {
// 技能全文一次注入上下文的长度上限（字符）。
// 当一个技能全文过长时，若整体作为 tool 结果返回会在单条消息内撑爆模型上下文
// （实测 cpp-code-review 等技能可到 1W+ token）。超过该上限即截断，
// 并在末尾附返回值：让模型在确实需要完整指令时用 FileRead 读取 SKILL.md。
constexpr std::size_t kMaxSkillTextLength = 20000;
}  // namespace

SkillTool::SkillTool(std::shared_ptr<command::CommandRegistry> registry)
    : registry_(std::move(registry)) {}

void SkillTool::set_registry(std::shared_ptr<command::CommandRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    registry_ = std::move(registry);
}

const std::string& SkillTool::name() const {
    static const std::string n{"Skill"};
    return n;
}

const std::string& SkillTool::description() const {
    static const std::string d{"Loads a skill by name and returns its full instructions."};
    return d;
}

const std::string& SkillTool::prompt() const {
    static const std::string p{
        "Loads the detailed instructions of a skill by its name. "
        "Use when the current task matches a skill's description or when_to_use."};
    return p;
}

nlohmann::json SkillTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {{"name", {{"type", "string"}, {"description", "The skill name to load"}}}}},
        {"required", {"name"}},
        {"additionalProperties", false}};
}

ResultV2<ToolResult> SkillTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    const auto name_it = input.find("name");
    if (name_it == input.end() || !name_it->is_string() || name_it->get<std::string>().empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Missing required argument: name");
    }

    std::shared_ptr<command::CommandRegistry> registry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        registry = registry_;
    }
    if (!registry) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Skill registry not configured");
    }

    const auto cmd = registry->find_by_name(name_it->get<std::string>());
    if (!cmd) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         "Skill not found: " + name_it->get<std::string>());
    }

    auto* prompt_cmd = dynamic_cast<command::PromptCommand*>(cmd.get());
    if (!prompt_cmd) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Not a skill: " + name_it->get<std::string>());
    }

    command::CommandContext cctx;
    cctx.cwd = ctx.cwd;
    cctx.model = ctx.model;
    cctx.session_id = ctx.session_id;

    // 复用共享取全文 helper（与 AgentTool 子 Agent skill 预加载共用，避免两处漂移）
    std::string text = agent::skill::build_skill_full_text(*prompt_cmd, cctx);

    // 超长技能截断，防止单条 tool 结果撑爆上下文（详见 kMaxSkillTextLength）
    const std::string skill_name = prompt_cmd->name();
    if (text.size() > kMaxSkillTextLength) {
        text.resize(kMaxSkillTextLength);
        text += "\n\n[技能 '" + skill_name +
                "' 全文过长已截断（仅保留前段）。如需完整指令，请用 FileRead 工具读取该技能 "
                "目录下的 SKILL.md 文件。]";
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(text)));
}

}  // namespace agent::tool
