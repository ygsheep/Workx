/**
 * @file i_agent_loop.h
 * @brief IAgentLoop — 统一 Agent 循环接口（#33 类型体系 / 里程碑 0.6.x）
 * @details 屏蔽不同 Agent 类型（ReAct / GoalGuarded / Planner / Coordinator …）
 *          的执行差异，供 QueryEngine 统一调用。所有实现都复用 ReActResult
 *          承载步骤历史/token/目标状态，UI 侧无需感知 Agent 类型差异。
 *
 *          现有 ReActLoop 保留其自有的 run() 主接口（向后兼容、零改动），
 *          本接口通过适配器包装暴露；新 Agent 类型直接实现 IAgentLoop。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <memory>
#include <atomic>

#include "agent/core/agent_type.h"
#include "agent/core/react_loop.h"    // ReActResult / ChatMessage
#include "agent/core/goal_verdict.h"  // AgentGoal

namespace agent {

/// @brief 一次 Agent 循环的输入（与 ChatSession run_completion 对齐）
struct AgentRunContext {
    /// P2-5：裸指针仅作视图，不拥有；生命周期契约——
    ///       调用方须保证该 vector 在 run() 返回前一直有效（含其内部元素），
    ///       Agent 循环会在目标场景下往末尾追加"继续"提示，故不得为 const，
    ///       且调用期间不能被并发改队列（由宿主持有消息锁）。
    std::vector<ChatMessage>* messages = nullptr;  ///< 会话历史（读写，见上契约）
    std::string system_prompt;
    nlohmann::json tools_schema;
    const std::atomic<bool>* should_cancel = nullptr;  ///< 空 = 永不取消；须 run 期间有效
    AgentGoal goal;                                    ///< 空 = 无目标守卫
    std::string goal_spec;               ///< agent.goal 原文（展示/事件透传用）
    IReActObserver* observer = nullptr;  ///< 空 = 不发布流式事件
};

/// @brief 一次 Agent 循环的结果
struct AgentRunResult {
    ReActResult react;  ///< 复用既有结果载体（步骤/token/目标状态）
    AgentType agent_type = AgentType::Unknown;  ///< 实际执行的类型
    /// Background 模式：已分发的后台任务 id（空 = 同步执行）
    std::string background_task_id;
};

/// @brief 统一 Agent 循环接口
class WORKX_API IAgentLoop {
   public:
    virtual ~IAgentLoop() = default;

    /// @brief 执行一轮 Agent 循环
    virtual AgentRunResult run(AgentRunContext ctx) = 0;

    /// @brief 本 Agent 类型
    virtual AgentType type() const noexcept = 0;
};

/// @brief 目标注入来源（QueryEngine 解析 agent.active 后，目标从何而来）
struct GoalResolver {
    /// 根据类型/配置产出目标；返回 None 表示无目标（普通对话）
    std::function<AgentGoal()> resolve;
};

/// @brief 永不触发的取消信号（adaptive 需要非空 should_cancel 时使用）
/// @details 返回静态原子的引用，生命周期同进程；用 == false 比对无副作用。
const std::atomic<bool>& kNeverCancel();

}  // namespace agent