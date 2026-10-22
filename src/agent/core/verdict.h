/**
 * @file verdict.h
 * @brief 目标验证器（#31 目标导向 Agent / 里程碑 0.6.x）
 * @details "验到成功为止"的关键：每次 ReAct 行动后，用验证器判定目标是否达成
 *          （Achieved / Pending / Failed）。验证器按 AgentGoal::Type 注册，
 *          复用 subprocess::exec() 执行命令（FileExists 走文件系统 stat）。
 * @version 1.1.0
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

    /// @brief #78 P4：本次验证**没有可用的验证手段**（探测不到命令）
    /// @details 与 Failed 的区别：Failed 是「跑了没过」，本项是「没得跑」。
    ///          门禁必须放行，否则模型会为一个不存在的目标反复纠错，
    ///          把「无法验证」误报成「验证失败」（VF-03 要的正是这点）。
    bool unavailable = false;
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

/// @brief #78 P4：在工作目录内探测某类目标可用的验证命令
/// @details 解决「MVP 无条件跑硬编码 ctest」的问题 —— 测试/构建入口随
///          技术栈而变，探测不到时返回空串，由验证器置 Verdict::unavailable
///          让门禁放行，而不是把「无法验证」误报成「验证失败」。
///
///          判定只看**项目线索文件**（CMakeLists.txt / package.json / Cargo.toml /
///          go.mod / Makefile / pytest 配置），不解析构建产物、不联网。
///          返回的命令串一定在白名单内（随后仍过 guard_command 复核）。
///
/// @param type 目标类型；None / FileExists / CustomScript 不参与探测（返回空串）
/// @param cwd  工作目录
/// @return 可用命令串；空串 = 该项目没有此类验证手段
std::string detect_goal_command(AgentGoal::Type type, const std::string& cwd);

/// @brief #78 P3：用户未声明 agent.goal 时，按工作目录推断一个默认验证目标
/// @details 优先级「能跑测试 > 能跑构建 > 无」。探测不到手段就返回 None，
///          门禁据此放行 —— 这正是「默认开启」的安全边界：
///          没有可跑命令的项目不会因为开了默认就被反复回灌。
/// @param cwd 工作目录
/// @return 推断出的目标；None 表示该项目没有可验证的东西
AgentGoal detect_default_goal(const std::string& cwd);

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