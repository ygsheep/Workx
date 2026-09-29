/**
 * @file script_agent.h
 * @brief ScriptAgent — 确定性脚本执行（#32 / 里程碑 0.6.x）
 * @details 与 GoalGuarded 的 CustomScript 目标不同：ScriptAgent 是独立 Agent
 *          模式（agent.active=script），执行一条确定性命令并立即返回结果，全程
 *          不调用 LLM。命令需通过 verdict 白名单（guard_command），防任意 RCE。
 *          结果复用 ReActResult 载体（含 goal_status 与 final_answer），保证
 *          UI / QueryTracker 无需感知 Agent 类型差异。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

#include "agent/core/goal_guarded_agent.h"  // GoalAgentDeps
#include "agent/core/goal_verdict.h"        // AgentGoal
#include "agent/core/react_loop.h"          // ReActResult / IReActObserver
#include "agent/api/chat_types.h"           // ChatMessage

namespace agent {

/// @brief 确定性脚本执行 Agent（无需 LLM）
class WORKX_API ScriptAgent {
   public:
    explicit ScriptAgent(GoalAgentDeps deps);
    ScriptAgent(const ScriptAgent&) = delete;
    ScriptAgent& operator=(const ScriptAgent&) = delete;

    /// @brief 执行 goal.command 并汇总结果
    /// @param goal      目标（须为 Script 类型，否则返回错误结果）
    /// @param goal_spec agent.goal 原文（事件/调用链透传，可空）
    /// @param messages  会话历史（追加以 result 汇总的 assistant 消息便于持久化）
    /// @param observer  事件观察者（当前仅透传，未发布流式事件；可空）
    /// @return ReActResult：exit 0 → goal_status=Achieved；非 0 → Failed；
    ///        命令被白名单拦截 → was_error=true 且回显拒绝说明。
    ReActResult run(const AgentGoal& goal, const std::string& goal_spec,
                    std::vector<ChatMessage>& messages, IReActObserver* observer);

   private:
    GoalAgentDeps m_deps;
};

}  // namespace agent