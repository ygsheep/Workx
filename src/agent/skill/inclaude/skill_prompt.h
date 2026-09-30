/**
 * @file skill_prompt.h
 * @brief Skills 列表 → system prompt 小节
 * @details 生成 "Available skills:" 段落（name + description + when_to_use），
 *          对应 example/cc 的 getSlashCommandToolSkills 过滤与注入。
 *          纯函数，header-only。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <optional>
#include <string>

#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/registry.h"

namespace agent::skill {

/// @brief 提取技能全文（多 PromptBlock 的 Text 部分拼接，#56 方案 C 共享 helper）
/// @details 复用于 SkillTool（加载技能）与 AgentTool（子 Agent 预加载 skill 到初始消息），
///          避免两处取全文逻辑漂移。仅取 Text 块，图片/工具结果块跳过。
/// @param cmd 技能对应的 PromptCommand（须为技能：loaded_from==Skills + type==prompt）
/// @param cctx 执行上下文（cwd/model/session_id，供技能内展开）
/// @return 技能全文；无 Text 块时返回空串
inline std::string build_skill_full_text(const command::PromptCommand& cmd,
                                         const command::CommandContext& cctx) {
    std::string text;
    for (const auto& block : cmd.generate_prompt("", cctx)) {
        if (block.type != command::PromptBlockType::Text) continue;
        if (!text.empty()) text += "\n";
        text += block.text;
    }
    return text;
}

/// @brief 构建 skills 提示词小节
/// @param registry 命令注册表
/// @param active_agent 当前 agent 名；声明了 agent 字段且不匹配的 skill 不注入（空 = 不过滤）
/// @return 形如 "\n\n# Available skills\n- <name>: <description>..." 的文本；
///         无模型可调用的 skill 时返回空串
inline std::string build_skills_prompt_section(
    const command::CommandRegistry& registry,
    const std::optional<std::string>& active_agent = std::nullopt) {
    std::string out;
    for (const auto& cmd : registry.get_by_type("prompt")) {
        if (cmd->loaded_from() != command::LoadSource::Skills) continue;
        if (cmd->is_model_invocation_disabled()) continue;
        // agent 过滤：声明了关联 agent 且与当前 agent 不符 → 不注入
        if (cmd->agent().has_value() && active_agent != cmd->agent()) continue;
        if (cmd->description().empty() && !cmd->when_to_use().has_value() &&
            !cmd->context().has_value())
            continue;
        // conditional skills（paths 非空）：不常驻 system prompt，命中才注入
        if (!cmd->paths().empty()) continue;

        out += "- ";
        out += cmd->name();
        out += ": ";
        if (!cmd->description().empty()) {
            out += cmd->description();
            if (cmd->when_to_use().has_value()) {
                out += " (when to use: " + cmd->when_to_use().value() + ")";
            }
            if (cmd->context().has_value()) {
                out += " (context: " + cmd->context().value() + ")";
            }
        } else if (cmd->when_to_use().has_value()) {
            out += cmd->when_to_use().value();
            if (cmd->context().has_value()) {
                out += " (context: " + cmd->context().value() + ")";
            }
        } else {
            out += cmd->context().value();
        }
        out += "\n";
    }
    if (out.empty()) return std::string{};
    return "\n\n# Available skills\nUse the Skill tool to load the full instructions of a "
           "skill.\n" +
           out;
}

}  // namespace agent::skill
