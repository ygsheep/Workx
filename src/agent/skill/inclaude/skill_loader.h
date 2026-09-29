/**
 * @file skill_loader.h
 * @brief Skill 磁盘加载器
 * @details 扫描 .claude/skills/<name>/SKILL.md，解析并生成 PromptCommand
 *          （对应 example/cc 的 loadSkillsFromSkillsDir）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/registry.h"

namespace agent::skill {

/// @brief 从 cwd 向上到根收集存在的 .claude/skills 目录
/// @param cwd 起点目录
/// @return 目录列表，cwd 最深优先（越近优先级越高）
std::vector<std::string> find_skill_dirs_up_to_home(const std::string& cwd);

/// @brief 收集用户级 skills 目录（home 不在 cwd 祖先路径链时的显式支持）
/// @return ~/.claude/skills 与 ~/.workx/skills 中存在的目录（该顺序，优先级低于项目链）
std::vector<std::string> find_user_skill_dirs();

/// @brief 从给定基础目录加载 skills
/// @param base_dirs 如 ["<cwd>/.claude/skills", ...]
/// @return PromptCommand 列表（含别名命令；同一 SKILL.md 仅加载一次）
/// @note 无 SKILL.md 的子目录跳过；读取/解析失败不影响其它 skill
/// @note 命令名（含别名）去重：同名仅保留首个，依赖 base_dirs 近目录优先的约定
std::vector<std::shared_ptr<command::PromptCommand>> load_skills_from_dirs(
    const std::vector<std::string>& base_dirs);

/// @brief 程序化注册单个内置 skill（bundled）
/// @param registry 目标命令注册表
/// @param skill_dir 含 SKILL.md 的目录
/// @return 注册的命令数（含别名）；无 SKILL.md 或解析失败返回 0
/// @note 命令标记 LoadSource::Bundled；若传入非空 seen_names，同名（含别名）跳过（首个注册优先）
size_t register_bundled_skill(command::CommandRegistry& registry, const std::string& skill_dir,
                              std::unordered_set<std::string>* seen_names = nullptr);

/// @brief 返回 bundled skills 根目录（<exe_dir>/skills/bundled）
/// @return 绝对路径；该目录不存在时返回空串（安装在未带 bundled skills 时优雅降级）
/// @note 与 ToolRegistry 的 bundled 资源约定一致（<exe_dir>/tools/*），
///       源码目录由 CMake POST_BUILD 拷贝到 <exe_dir>/skills/bundled
std::string find_bundled_skills_dir();

/// @brief 注册 root 下全部 bundled skills（root/<name>/SKILL.md）
/// @param registry 目标命令注册表
/// @param root bundled skills 根目录
/// @return 注册的命令总数（含别名）；无子目录或全空返回 0
/// @note 逐项复用 register_bundled_skill，跨技能用 seen_names 去重（首个注册优先，
///       与 load_skills_from_dirs 语义一致）
size_t register_bundled_skills(command::CommandRegistry& registry, const std::string& root);

}  // namespace agent::skill
