/**
 * @file batch_agent.h
 * @brief BatchAgent — 对同构输入并行执行（#32 / 里程碑 0.6.x）
 * @details 独立 Agent 模式（agent.active=batch）：按 glob 展开一组成员，对每个
 *          成员物化同一命令模板（{item} 占位），以并发度并行执行，汇总结果。
 *          全程不调用 LLM。命令需过 verdict 白名单，item 需可安全 shell 引用。
 *          所有成员 exit 0 视为 Achieved，否则 Failed（逐条回显）。
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

/// @brief 同构批处理 Agent（无需 LLM）
class WORKX_API BatchAgent {
   public:
    explicit BatchAgent(GoalAgentDeps deps);
    BatchAgent(const BatchAgent&) = delete;
    BatchAgent& operator=(const BatchAgent&) = delete;

    /// @brief glob 展开 → 并行物料化执行 → 汇总结果
    /// @param goal      目标（须为 Batch 类型，command 为含 {item} 的模板）
    /// @param goal_spec agent.goal 原文（透传，可空）
    /// @param messages  会话历史（追加以汇总的 assistant 消息便于持久化）
    /// @param observer  观察者（透传，可空）
    ReActResult run(const AgentGoal& goal, const std::string& goal_spec,
                    std::vector<ChatMessage>& messages, IReActObserver* observer);

   private:
    GoalAgentDeps m_deps;
};

}  // namespace agent