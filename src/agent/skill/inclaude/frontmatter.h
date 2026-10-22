/**
 * @file frontmatter.h
 * @brief Skill 文件 frontmatter 解析
 * @details 解析 .claude/skills/<name>/SKILL.md 的 YAML 风格 frontmatter 头
 *          （仅扁平 key: value，不支持嵌套），分离正文
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

namespace agent::skill {

/// @brief Skill frontmatter 字段 — 对应 example/cc 的 parseSkillFrontmatterFields
struct SkillFrontmatter {
    std::string name;                          ///< 技能名称（缺省用目录名）
    std::string description;                   ///< 描述（缺省用正文首行派生）
    std::vector<std::string> aliases;          ///< 别名（逗号分隔）
    std::optional<std::string> argument_hint;  ///< 参数提示
    std::optional<std::string> when_to_use;    ///< 使用场景（注入 system prompt 用）
    std::optional<std::string>
        context;  ///< 适用上下文（注入 system prompt 用，与 when_to_use 并列展示）
    std::optional<std::string> agent;  ///< 关联 agent 声明（非空时仅匹配当前 agent 才注入/激活）
    std::vector<std::string> hooks;  ///< PreActivate 钩子命令（激活时执行，多行 - cmd 或逗号分隔）
    std::vector<std::string> hooks_json;  ///< 对象式通用 Hook（`- event: PreToolUse ...`，序列化为
                                          ///< JSON，Skill 激活时注册）
    std::optional<std::string> model;      ///< 指定模型
    bool user_invocable{true};             ///< 是否用户可调用（/name）
    bool disable_model_invocation{false};  ///< 是否禁止模型自动调用
    std::vector<std::string> paths;  ///< conditional 触发 glob（touch 匹配文件时激活）
};

/// @brief 解析结果：frontmatter + 正文
struct ParsedSkill {
    SkillFrontmatter frontmatter;
    std::string body;  ///< 正文（不含 frontmatter 分隔符）
};

/// @brief 解析 SKILL.md 内容（纯函数，无 I/O）
/// @param content 文件内容（UTF-8）
/// @param default_name 目录名，frontmatter 缺省 name 时使用
/// @note frontmatter 块必须位于文件开头（`---` 包裹）；无 frontmatter 时
///       所有字段取默认值，正文为整个内容
ParsedSkill parse_skill_content(const std::string& content, const std::string& default_name);

}  // namespace agent::skill
