/**
 * @file agent_loop_adapters.h
 * @brief IAgentLoop 适配器（#33 类型体系 / 里程碑 0.6.x）
 * @details 将现有 ReActLoop（保留自身 run() 零改动）与 GoalGuardedAgent
 *          包装为统一 IAgentLoop 接口，供 QueryEngine 按 AgentType 路由调用。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/i_agent_loop.h"
#include "agent/core/react_loop.h"
#include "agent/core/goal_guarded_agent.h"
#include "agent/core/role_agent.h"
#include "agent/core/script_agent.h"
#include "agent/core/batch_agent.h"
#include "agent/core/watch_agent.h"

namespace agent {

/// @brief 现有 ReActLoop 的 IAgentLoop 适配（默认通用对话）
/// @details 持有 ReActLoop 实例（由 QueryEngine 按 Celia deps 构造），
///          包装其 run() 为 IAgentLoop::run()。agent_type = ReAct。
class WORKX_API ReActLoopAdapter final : public IAgentLoop {
   public:
    explicit ReActLoopAdapter(std::unique_ptr<ReActLoop> loop);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::ReAct; }

   private:
    std::unique_ptr<ReActLoop> m_loop;
};

/// @brief GoalGuardedAgent 的 IAgentLoop 适配（目标驱动）
/// @details agent_type = GoalGuarded；仅当 ctx.goal 非空时才进入目标环，
///          否则退化为单轮 ReAct（由 GoalGuardedAgent 自身处理）。
class WORKX_API GoalGuardedLoopAdapter final : public IAgentLoop {
   public:
    explicit GoalGuardedLoopAdapter(GoalAgentDeps deps);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::GoalGuarded; }

   private:
    GoalAgentDeps m_deps;
};

/// @brief ScriptAgent 的 IAgentLoop 适配（#32 确定性脚本）
/// @details agent_type = Script；要求 ctx.goal.type==Script，否则 ScriptAgent
///          返回错误结果（won't crash）。
class WORKX_API ScriptLoopAdapter final : public IAgentLoop {
   public:
    explicit ScriptLoopAdapter(GoalAgentDeps deps);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::Script; }

   private:
    GoalAgentDeps m_deps;
};

/// @brief BatchAgent 的 IAgentLoop 适配（#32 同构并行）
class WORKX_API BatchLoopAdapter final : public IAgentLoop {
   public:
    explicit BatchLoopAdapter(GoalAgentDeps deps);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::Batch; }

   private:
    GoalAgentDeps m_deps;
};

/// @brief WatchAgent 的 IAgentLoop 适配（#32 文件/事件监控）
class WORKX_API WatchLoopAdapter final : public IAgentLoop {
   public:
    explicit WatchLoopAdapter(GoalAgentDeps deps);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::Watch; }

   private:
    GoalAgentDeps m_deps;
};

/// @brief 角色 Agent 的 IAgentLoop 适配（#33 Planner/Executor/Coordinator/Researcher/Reviewer）
/// @details 构造时由 QueryEngine 指定具体的 AgentType，RoleAgent 据此加载对应
///          RoleProfile（角色指令 + 工具过滤策略）。
class WORKX_API RoleLoopAdapter final : public IAgentLoop {
   public:
    RoleLoopAdapter(GoalAgentDeps deps, AgentType type);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return m_type; }

   private:
    GoalAgentDeps m_deps;
    AgentType m_type;
};

/// @brief BackgroundAgent 的 IAgentLoop 适配（长时运行、不阻塞主对话、事件通知）
/// @details agent.active="background" 时由 QueryEngine 路由到此。run() 把整条用户
///          请求包装成 TaskManager 后台任务（底层默认 ReAct 循环）并**立即返回**
///          task_id 提示；后台线程逐步发 BackgroundProgressEvent、完成发
///          BackgroundCompletedEvent。不触碰主会话 m_messages（run() 返回后消息
///          未变，任务在独立消息缓冲中运行）。
/// @note 单一实例只分发一次（m_used 单次使用守卫，防并发 run() 对 m_task_id 竞争）。
class WORKX_API BackgroundLoopAdapter final : public IAgentLoop {
   public:
    explicit BackgroundLoopAdapter(const GoalAgentDeps& deps);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::Background; }

    /// 最近一次分发的后台任务 id；按值返回避免对外暴露内部缓冲的引用竞争
    std::string task_id() const noexcept { return m_task_id; }
    /// 定向取消本实例分发的后台任务（task_manager->cancel；未分发/已结束为空操作）
    void cancel() const noexcept;

   private:
    GoalAgentDeps m_deps;
    std::atomic<bool> m_used{false};  ///< 单次使用守卫（PR R3-1 P1-2）
    std::string m_task_id;  ///< 最近一次分发的后台任务 id（写入受 m_used 保护）
};

}  // namespace agent