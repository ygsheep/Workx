#include "agent/core/agent_type.h"

#include <algorithm>
#include <cctype>

namespace agent {

namespace {

/// @brief 去掉首尾空白并转小写（ASCII），供前缀匹配
std::string normalized(std::string_view s) {
    std::string out(s);
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(out.begin(), out.end(), not_space);
    auto last = std::find_if(out.rbegin(), out.rend(), not_space).base();
    if (first >= last) {
        out.clear();
        return out;
    }
    out = std::string(first, last);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

}  // namespace

AgentType parse_agent_type(std::string_view s) noexcept {
    const std::string v = normalized(s);
    if (v.empty() || v == "react" || v == "explore") {
        return AgentType::ReAct;
    }
    if (v == "goal-guarded" || v == "goalguarded" || v == "verify") {
        return AgentType::GoalGuarded;
    }
    if (v == "planner" || v == "plan") return AgentType::Planner;
    if (v == "executor" || v == "execute") return AgentType::Executor;
    if (v == "coordinator" || v == "coordinate") return AgentType::Coordinator;
    if (v == "researcher" || v == "research") return AgentType::Researcher;
    if (v == "reviewer" || v == "review") return AgentType::Reviewer;
    if (v == "batch") return AgentType::Batch;
    if (v == "watch" || v == "watcher") return AgentType::Watch;
    if (v == "script" || v == "script-agent") return AgentType::Script;
    if (v == "background" || v == "bg") return AgentType::Background;
    return AgentType::Unknown;
}

std::string_view to_string(AgentType type) noexcept {
    switch (type) {
        case AgentType::ReAct:
            return "react";
        case AgentType::GoalGuarded:
            return "goal-guarded";
        case AgentType::Planner:
            return "planner";
        case AgentType::Executor:
            return "executor";
        case AgentType::Coordinator:
            return "coordinator";
        case AgentType::Researcher:
            return "researcher";
        case AgentType::Reviewer:
            return "reviewer";
        case AgentType::Batch:
            return "batch";
        case AgentType::Watch:
            return "watch";
        case AgentType::Script:
            return "script";
        case AgentType::Background:
            return "background";
        case AgentType::Unknown:
        default:
            return "unknown";
    }
}

bool is_implemented(AgentType type) noexcept {
    switch (type) {
        case AgentType::ReAct:
        case AgentType::GoalGuarded:
        case AgentType::Batch:  // #32 多模式已实现
        case AgentType::Watch:
        case AgentType::Script:
        case AgentType::Planner:  // #33 角色 Agent 已实现
        case AgentType::Executor:
        case AgentType::Coordinator:
        case AgentType::Researcher:
        case AgentType::Reviewer:
        case AgentType::Background:  // 后台分发（包装默认底层 Agent）
            return true;
        default:
            return false;
    }
}

}  // namespace agent