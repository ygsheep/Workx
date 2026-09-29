/**
 * @file role_agent.h
 * @brief RoleAgent — #33 角色 Agent（Planner/Executor/Coordinator/Researcher/Reviewer）
 * @details 五个角色本质是"带角色指令 + 工具集约束"的 ReActLoop 变体，差异仅在
 *          - 系统提示追加一段角色行为约束
 *          - 工具 schema 子集过滤（只读角色仅暴露只读工具；Coordinator 放行 AgentTool）
 *          故用"一个 RoleAgent + RoleProfile 参数化"实现，避免五个近似重复类。
 *          复用手头 ReActLoop（经 ReActLoopFactory::make 注入权限/事件/压缩器。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/core/i_agent_loop.h"
#include "agent/core/goal_guarded_agent.h"  // GoalAgentDeps / ReActLoopFactory

namespace agent {

/// @brief 角色定义：提示覆盖 + 工具过滤策略
struct RoleProfile {
    AgentType type = AgentType::Unknown;  ///< 本角色对应的 AgentType
    std::string_view name;                ///< 角色名（日志/提示用）
    std::string_view instruction;         ///< 追加到 system_prompt 的角色行为约束
    bool read_only = false;               ///< 只读角色：仅暴露 is_read_only 工具
    bool keep_agent_tool = false;         ///< 放行 AgentTool（Coordinator 编排需要）
};

/// @brief 五种 #33 角色的内置规格（name 英文：planner/executor/coordinator/researcher/reviewer）
const RoleProfile& role_profile_of(AgentType type) noexcept;

/// @brief 角色 Agent：按角色过滤工具子集 + 追加角色指令，驱动一个 ReActLoop
class WORKX_API RoleAgent {
   public:
    explicit RoleAgent(GoalAgentDeps deps, RoleProfile profile);

    /// @brief 执行一轮角色 ReAct
    /// @param ctx 上下文；system_prompt 会被追加角色指令；tools_schema 会被
    ///            按角色过滤（只读/放行 AgentTool），过滤后实际传给 LLM
    /// @return AgentRunResult（agent_type = 角色类型）
    AgentRunResult run(AgentRunContext ctx);

    AgentType type() const noexcept { return m_profile.type; }

   private:
    /// @brief 依据角色策略从全量 registry 过滤出子集 schema
    nlohmann::json filtered_schema() const;

    GoalAgentDeps m_deps;
    RoleProfile m_profile;
};

}  // namespace agent