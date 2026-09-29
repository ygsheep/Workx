/**
 * @file conditional.h
 * @brief Conditional Skills 支持：touch 收集与激活匹配
 * @details touch 路径（用户消息 /file 引用 + 工具上报）与 skill 的
 *          paths glob 匹配，命中即激活该 skill。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "agent/command/inclaude/command.h"

namespace agent::skill {

/// @brief 会话级 touch 路径收集器（线程安全）
/// @details 工具并行执行时可能同时上报，内部加锁；
///          路径做 weakly_canonical 绝对化，重复路径自动去重。
class TouchCollector {
   public:
    /// @brief 记录一个被 touch 的路径（空路径忽略）
    void add(const std::string& path);

    /// @brief 当前所有已 touch 路径快照
    std::vector<std::string> paths() const;

    /// @brief 清空收集器
    void clear();

   private:
    mutable std::mutex m_mutex;
    std::unordered_set<std::string> m_paths;
};

/// @brief 匹配激活 conditional skills
/// @param touched 已 touch 的绝对路径列表
/// @param skills 候选命令（paths 为空或非 PromptCommand 者永不匹配）
/// @param cwd 工作目录（相对 pattern 的基准）
/// @return 命中的 PromptCommand（按输入顺序去重）
std::vector<std::shared_ptr<command::PromptCommand>> activate_conditional_skills(
    const std::vector<std::string>& touched,
    const std::vector<std::shared_ptr<command::CommandBase>>& skills, const std::string& cwd);

/// @brief 单个路径是否命中某命令的 paths
/// @param path 绝对路径
/// @param skill 命令（读取 paths）
/// @param cwd 工作目录（相对 pattern 的基准）
bool skill_matches_touch(const std::string& path, const command::CommandBase& skill,
                         const std::string& cwd);

}  // namespace agent::skill
