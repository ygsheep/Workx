/**
 * @file watch_agent.h
 * @brief WatchAgent — 路径/事件监控（#32 / 里程碑 0.6.x）
 * @details 独立 Agent 模式（agent.active=watch）：在目标目录内以 poll 方式监控
 *          内容变化（快照签名：大小+mtime），检测到变化后触发命令模板执行。
 *          首轮仅建立基线，之后每轮与上一基线比对，变化则执行命令并刷新基线。
 *          轮询次数有界（max_polls），本轮结束即退出（不在单次用户 turn 内长时间挂起）。
 *          全程不调用 LLM；命令需过 verdict 白名单。
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

/// @brief 文件/事件监控 Agent（无需 LLM）
class WORKX_API WatchAgent {
   public:
    explicit WatchAgent(GoalAgentDeps deps);
    WatchAgent(const WatchAgent&) = delete;
    WatchAgent& operator=(const WatchAgent&) = delete;

    /// @brief 有界轮询监控并触发命令
    /// @param goal      目标（须为 Watch 类型）
    /// @param goal_spec agent.goal 原文（透传，可空）
    /// @param messages  会话历史
    /// @param observer  观察者（透传，可空）
    ReActResult run(const AgentGoal& goal, const std::string& goal_spec,
                    std::vector<ChatMessage>& messages, IReActObserver* observer);

   private:
    GoalAgentDeps m_deps;
};

}  // namespace agent