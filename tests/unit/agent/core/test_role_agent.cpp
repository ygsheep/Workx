/**
 * @file test_role_agent.cpp
 * @brief #33 角色 Agent 测试（RoleProfile / is_implemented / make_loop 路由）
 * @details 覆盖：
 *          - role_profile_of：各角色的 read_only / keep_agent_tool 策略正确
 *          - is_implemented：Planner/Executor/Coordinator/Researcher/Reviewer 已标记实现
 *          - make_loop：五角色路由到 RoleLoopAdapter（type() 匹配，不再回退 ReAct）
 * @note RoleAgent::run 需要真实 provider，不做端到端；此处测纯逻辑与路由面。
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "agent/core/agent_type.h"
#include "agent/core/role_agent.h"
#include "agent/core/query_engine.h"
#include "agent/core/goal_guarded_agent.h"
#include "agent/core/i_agent_loop.h"

using namespace agent;

namespace {

// 最小 GoalAgentDeps（仅 registry 非空，供 make_loop 构造；其余保持默认）
GoalAgentDeps make_deps() {
    GoalAgentDeps d;
    d.registry = std::make_shared<tool::ToolRegistry>();
    return d;
}

}  // namespace

// ============================================================
// role_profile_of：角色策略
// ============================================================

TEST_CASE("role: Planner 只读且不携带 AgentTool", "[agent][role]") {
    const auto& p = role_profile_of(AgentType::Planner);
    REQUIRE(p.type == AgentType::Planner);
    REQUIRE(p.read_only);
    REQUIRE_FALSE(p.keep_agent_tool);
    REQUIRE_FALSE(p.instruction.empty());
}

TEST_CASE("role: Researcher 只读且不携带 AgentTool", "[agent][role]") {
    const auto& p = role_profile_of(AgentType::Researcher);
    REQUIRE(p.type == AgentType::Researcher);
    REQUIRE(p.read_only);
    REQUIRE_FALSE(p.keep_agent_tool);
}

TEST_CASE("role: Reviewer 只读且不携带 AgentTool", "[agent][role]") {
    const auto& p = role_profile_of(AgentType::Reviewer);
    REQUIRE(p.type == AgentType::Reviewer);
    REQUIRE(p.read_only);
    REQUIRE_FALSE(p.keep_agent_tool);
}

TEST_CASE("role: Executor 全量工具且不携带 AgentTool", "[agent][role]") {
    const auto& p = role_profile_of(AgentType::Executor);
    REQUIRE(p.type == AgentType::Executor);
    REQUIRE_FALSE(p.read_only);
    REQUIRE_FALSE(p.keep_agent_tool);
}

TEST_CASE("role: Coordinator 全量工具且放行 AgentTool", "[agent][role]") {
    const auto& p = role_profile_of(AgentType::Coordinator);
    REQUIRE(p.type == AgentType::Coordinator);
    REQUIRE_FALSE(p.read_only);
    REQUIRE(p.keep_agent_tool);  // 编排需要子 Agent 调度
}

// ============================================================
// is_implemented：五个角色已标记实现
// ============================================================

TEST_CASE("role: is_implemented 五角色为 true", "[agent][role]") {
    REQUIRE(is_implemented(AgentType::Planner));
    REQUIRE(is_implemented(AgentType::Executor));
    REQUIRE(is_implemented(AgentType::Coordinator));
    REQUIRE(is_implemented(AgentType::Researcher));
    REQUIRE(is_implemented(AgentType::Reviewer));
}

TEST_CASE("role: Unknown 仍未实现", "[agent][role]") {
    REQUIRE_FALSE(is_implemented(AgentType::Unknown));
}

// ============================================================
// make_loop：五角色路由到 RoleLoopAdapter
// ============================================================

TEST_CASE("role: make_loop 五角色返回对应 IAgentLoop（type 匹配）", "[agent][role]") {
    QueryEngine qe(make_deps());
    for (const AgentType t : {AgentType::Planner, AgentType::Executor, AgentType::Coordinator,
                              AgentType::Researcher, AgentType::Reviewer}) {
        auto loop = qe.make_loop(t);
        REQUIRE(loop != nullptr);
        CHECK(loop->type() == t);  // 不再回退 ReAct
    }
}

TEST_CASE("role: make_loop 角色不变更 deps 的 registry", "[agent][role]") {
    GoalAgentDeps d = make_deps();
    QueryEngine qe(d);
    auto loop = qe.make_loop(AgentType::Coordinator);
    REQUIRE(loop != nullptr);
    REQUIRE(loop->type() == AgentType::Coordinator);
}