#include "agent/core/role_agent.h"

#include <utility>

#include "agent/core/agent_type.h"
#include "agent/tool/AgentTool/agent_tool.h"  // kAgentToolName（Coordinator 放行判断）
#include "liblogger/logger.h"

namespace agent {
namespace {

// 只读工具判定：itool 提供 is_read_only()；AgentTool 默认非只读（会启动子 Agent）
bool should_include(const std::shared_ptr<agent::tool::ITool>& t, const RoleProfile& p) {
    if (t->name() == agent::tool::kAgentToolName) {
        return p.keep_agent_tool;  // 仅 Coordinator 放行
    }
    if (p.read_only) {
        return t->is_read_only();  // 只读角色只留读工具
    }
    return true;
}

}  // namespace

const RoleProfile& role_profile_of(AgentType type) noexcept {
    static const RoleProfile kPlanner{
        .type = AgentType::Planner,
        .name = "planner",
        .instruction =
            "You are the Planner agent. Your job is to produce a clear, "
            "sequential plan for the task before any code is written. "
            "You are READ-ONLY: never modify files or run side-effecting "
            "commands. Investigate with read-only tools, then present a "
            "step-by-step plan and wait for confirmation.",
        .read_only = true,
        .keep_agent_tool = false,
    };
    static const RoleProfile kExecutor{
        .type = AgentType::Executor,
        .name = "executor",
        .instruction =
            "You are the Executor agent. A plan has already been agreed. "
            "Follow it strictly, step by step, using the available tools. "
            "Do not expand scope or invent new work beyond the plan; if the "
            "plan is ambiguous, state the assumption and proceed conservatively.",
        .read_only = false,
        .keep_agent_tool = false,
    };
    static const RoleProfile kCoordinator{
        .type = AgentType::Coordinator,
        .name = "coordinator",
        .instruction =
            "You are the Coordinator agent. Break the task into independent "
            "sub-tasks and dispatch them to sub-agents via the Agent tool "
            "(ideally in parallel), then aggregate their results into a "
            "coherent final answer. You may also inspect the workspace "
            "yourself, but prefer delegation for independent work.",
        .read_only = false,
        .keep_agent_tool = true,
    };
    static const RoleProfile kResearcher{
        .type = AgentType::Researcher,
        .name = "researcher",
        .instruction =
            "You are the Researcher agent. Investigate the question by "
            "collecting information (search / read / fetch), compare the "
            "alternatives, and produce a STRUCTURED comparison report with "
            "sources and a recommendation. You are READ-ONLY: never modify "
            "files or run side-effecting commands.",
        .read_only = true,
        .keep_agent_tool = false,
    };
    static const RoleProfile kReviewer{
        .type = AgentType::Reviewer,
        .name = "reviewer",
        .instruction =
            "You are the Reviewer agent. Review the provided code or plan "
            "with a critical eye. You are READ-ONLY: never modify files. "
            "List concrete issues (severity-ranked), plus suggestions, and "
            "a verdict on whether it is ready.",
        .read_only = true,
        .keep_agent_tool = false,
    };

    switch (type) {
        case AgentType::Planner:
            return kPlanner;
        case AgentType::Executor:
            return kExecutor;
        case AgentType::Coordinator:
            return kCoordinator;
        case AgentType::Researcher:
            return kResearcher;
        case AgentType::Reviewer:
            return kReviewer;
        default:
            return kPlanner;  // 理论不可达（仅五角色调用）
    }
}

RoleAgent::RoleAgent(GoalAgentDeps deps, RoleProfile profile)
    : m_deps(std::move(deps)), m_profile(profile) {}

nlohmann::json RoleAgent::filtered_schema() const {
    nlohmann::json schema = nlohmann::json::array();
    if (!m_deps.registry) {
        return schema;
    }
    for (const auto& t : m_deps.registry->get_all_tools()) {
        if (!should_include(t, m_profile)) {
            continue;
        }
        schema.push_back({
            {"name", t->name()},
            {"description", t->description()},
            {"input_schema", t->input_schema()},
        });
    }
    return schema;
}

AgentRunResult RoleAgent::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = m_profile.type;

    // 追加角色约束到系统提示
    std::string sys = ctx.system_prompt;
    if (!sys.empty()) {
        sys += "\n\n";
    }
    sys.append(m_profile.instruction.begin(), m_profile.instruction.end());

    // 按角色过滤工具子集
    const nlohmann::json schema = filtered_schema();
    LOG_DEBUG("[role_agent] type={} tools={}", to_string(m_profile.type),
              schema.is_array() ? schema.size() : 0);

    auto loop = ReActLoopFactory::make(m_deps, m_deps.registry, ReActLoop::Config{});
    out.react = loop->run(*ctx.messages, sys, schema,
                          ctx.should_cancel ? *ctx.should_cancel : kNeverCancel(), ctx.observer);
    return out;
}

}  // namespace agent