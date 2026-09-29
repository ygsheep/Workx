/**
 * @file verdict.h
 * @brief 目标验证器（#31 目标导向 Agent / 里程碑 0.6.x）
 * @details "验到成功为止"的关键：每次 ReAct 行动后，用验证器判定目标是否达成
 *          （Achieved / Pending / Failed）。验证器按 AgentGoal::Type 注册，
 *          复用 subprocess::exec() 执行命令（FileExists 走文件系统 stat）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <string_view>

#include "agent/core/goal_verdict.h"

namespace tool {
struct ToolContext;
}  // namespace tool

namespace agent {

/// @brief 单次验证结果
struct Verdict {
    GoalStatus status =
        GoalStatus::Pending;  ///< Achieved（达成）/ Pending（未达成）/ Failed（硬错误）
    std::string detail;  ///< 人可读的说明（例如测试失败数、文件缺失路径）
};

/// @brief 验证器函数：判定目标是否达成
/// @param goal 目标定义
/// @param cwd  工作目录（执行命令的基准目录）
/// @return 验证结果
using VerdictChecker = Verdict (*)(const AgentGoal& goal, const std::string& cwd);

/// @brief 基于 AgentGoal::Type 选择并执行对应验证器
/// @param goal 目标定义（None 时返回 Pending + "no goal"）
/// @param cwd  工作目录
Verdict check_goal(const AgentGoal& goal, const std::string& cwd);

/// @brief 查询某类型是否已有验证器实现
bool has_checker(AgentGoal::Type type) noexcept;

/// @brief 白名单校验待执行命令
/// @param cmd 命令串（默认命令 or 用户覆盖/模板命令）
/// @return 允许则返回原命令；被拦截返回空
/// @note #32：Guard_command 同时供 GoalGuarded 的 verify 与多模式 Agent
///       （Script/Batch/Watch）复用，统一"命令安全"落点，避免各自实现漂移。
std::string guard_command(const std::string& cmd);

namespace detail {
/// @brief 解析 stdout 中 enum 值（供大文件/失败子串判定用），供内置 checker 复用
int exit_code_of(const std::string& cmd, const std::string& cwd);
}  // namespace detail

}  // namespace agent