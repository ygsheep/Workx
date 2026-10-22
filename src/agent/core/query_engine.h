/**
 * @file query_engine.h
 * @brief QueryEngine — Agent 会话生命周期编排器（里程碑 0.6.x）
 * @details 从注释 stub 落为实体：作为唯一编排入口，按 agent.active 解析
 *          AgentType，构造对应 IAgentLoop（ReAct 适配器 / GoalGuarded 适配器），
 *          注入统一下游依赖并执行一轮查询。职责收拢自 ChatSession::run_completion
 *          的"建 loop 段"，使新 Agent 类型只需实现 IAgentLoop 即可接入。
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>

#include "agent/core/agent_type.h"
#include "agent/core/goal_guarded_agent.h"  // GoalAgentDeps / ReActLoopFactory
#include "agent/core/i_agent_loop.h"
#include "agent/core/query_tracker.h"
#include "agent/tool/registry.h"
#include "core/export.h"

namespace agent {

class IConfigManager;

/// @brief 会话级权限动作（QueryEngine 注入到各 Agent 循环 #28）
struct PermissionSnapshot {
    tool::PermissionMode mode{tool::PermissionMode::Default};
    tool::PermissionMode before_plan{tool::PermissionMode::Default};
    std::function<void(tool::PermissionMode, tool::PermissionMode, bool)> on_changed;
};

/// @brief 查询引擎：解析 AgentType → 构造 Agent → 执行（含 queryTracking 调用链）
class WORKX_API QueryEngine {
   public:
    /// @brief 依赖（与 GoalAgentDeps 同源，ReAct 与 GoalGuarded 共用）
    explicit QueryEngine(GoalAgentDeps deps);

    /// @brief agent.active → AgentType（含 skill 过滤兼容，见 parse_agent_type）
    AgentType resolve_agent_type(const IConfigManager& cfg) const;

    /// @brief 注入会话级权限状态（Default/Plan/Bypass），落到内部循环
    void set_permission(PermissionSnapshot snap);

    /// @brief 注入会话工作模式（标准/计划/极简），落到内部循环
    /// @details 极简模式同时由 ChatSession 侧过滤工具 schema（对 LLM 不可见），
    ///          此处注入供 ReActLoop 构造 ToolContext，Executor 做白名单守卫。
    void set_session_mode(tool::SessionMode mode) { m_deps.session_mode = mode; }

    /// @brief 构造并注册一种 Agent 类型的执行器（未实现/未知返回 nullptr）
    /// @param type 目标类型（ReAct / GoalGuarded 落地；其余返回 nullptr 占位）
    std::unique_ptr<IAgentLoop> make_loop(AgentType type) const;

    /// @brief 便捷：按配置解析类型并执行一轮查询（记录 QueryTracker）
    /// @param cfg 配置管理器（读 agent.active）
    /// @param ctx 查询上下文（含目标/目标原文/观察者）
    /// @return 执行结果；类型未知回退 ReAct
    AgentRunResult run(const IConfigManager& cfg, AgentRunContext ctx);

    /// @brief 查询调用链追踪器（begin/verdict/finish 记录，供 UI/诊断读取）
    const QueryTracker& tracker() const noexcept { return m_tracker; }

   private:
    void apply_permission(std::unique_ptr<ReActLoop>& loop) const;

    /// @brief 构建 ReAct 循环配置（读取 agent.max_iterations，缺失用引擎默认值）
    ReActLoop::Config make_react_config() const;

    GoalAgentDeps m_deps;
    QueryTracker m_tracker;
};

}  // namespace agent