#include "agent/core/goal_guarded_agent.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/query_tracker.h"
#include "agent/core/verdict.h"        // Verdict / check_goal
#include "core/events/agent_events.h"  // AgentVerdictEvent
#include "liblogger/logger.h"

namespace agent {

std::unique_ptr<ReActLoop> ReActLoopFactory::make(const GoalAgentDeps& deps,
                                                  std::shared_ptr<tool::ToolRegistry> registry,
                                                  ReActLoop::Config cfg) {
    auto loop = std::make_unique<ReActLoop>(
        deps.provider, std::move(registry), std::move(cfg), deps.config_manager, deps.task_manager,
        deps.cwd, deps.external_compactor, deps.event_bus, deps.touch_collector,
        deps.file_index_invalidator, deps.session_id, deps.queue_inject_cb);
    // 0.6.x：把会话级权限三态应用到循环 + 注册变更回调，
    // 修复路由（此前权限状态只在 chat_session 手动 ReAct 分支上应用）的缺口。
    loop->apply_permission_state(deps.permission_mode, deps.permission_mode_before_plan,
                                 deps.permission_mode == tool::PermissionMode::Plan);
    // 会话工作模式（标准/计划/极简）注入循环（极简模式白名单守卫依据）
    loop->set_session_mode(deps.session_mode);
    if (deps.permission_state_changed_cb) {
        loop->set_permission_state_changed_callback(deps.permission_state_changed_cb);
    }
    // #56 方案 C：注入命令注册表 → ToolContext.command_registry_ptr（子 Agent skill 预加载）
    loop->set_command_registry(deps.command_registry);
    // #56 方案 D：注入父会话全局 MCP 管理器 → ToolContext.mcp_manager_ptr
    loop->set_mcp_manager(deps.mcp_manager);
    return loop;
}

std::unique_ptr<ReActLoop> ReActLoopFactory::make_one_step(const GoalAgentDeps& deps) {
    ReActLoop::Config cfg;
    cfg.max_iterations = 1;
    return make(deps, deps.registry, std::move(cfg));
}

GoalGuardedAgent::GoalGuardedAgent(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

ReActResult GoalGuardedAgent::run(std::vector<ChatMessage>& messages,
                                  const std::string& system_prompt,
                                  const nlohmann::json& tools_schema,
                                  const std::atomic<bool>& should_cancel, const AgentGoal& goal,
                                  const std::string& goal_spec, IReActObserver* observer) {
    // 每轮 Verdict 结果 -> 事件（TUI）+ QueryTracker（调用链），双路但同源
    const auto emit_verdict = [&](const Verdict& v, int attempt) {
        if (m_deps.tracker) {
            m_deps.tracker->record_verdict(v.status, v.detail);
        }
        if (!m_deps.event_bus) {
            return;
        }
        AgentVerdictEvent event;
        event.session_id = m_deps.session_id;
        event.goal_spec = goal_spec;
        event.attempt = attempt;
        event.goal_status = static_cast<int32_t>(v.status);
        event.detail = v.detail;
        m_deps.event_bus->publish_async(std::move(event));
    };

    // P1-2：check_goal 可能因 filesystem::exists / exec 抛异常，若沿调用链上抛
    //        会直接崩掉整个会话（仅剩最外层 Fatal catch）。这里收敛为 Failed
    //        Verdict，让 GoalGuarded 循环安全继续/退出，而不是带崩会话。
    const auto safe_check_goal = [&](const AgentGoal& g, int attempt) -> Verdict {
        Verdict v;
        try {
            v = check_goal(g, m_deps.cwd);
        } catch (const std::exception& e) {
            v = Verdict{GoalStatus::Failed, std::format("checker error: {}", e.what())};
            LOG_ERROR("[goal_guarded] check_goal threw at attempt={}: {}", attempt, e.what());
        } catch (...) {
            v = Verdict{GoalStatus::Failed, "checker error: unknown exception"};
            LOG_ERROR("[goal_guarded] check_goal threw unknown at attempt={}", attempt);
        }
        emit_verdict(v, attempt);
        return v;
    };

    // 无目标：退化为单轮 ReAct，直接返回（不循环）
    if (!goal.has_goal()) {
        return ReActLoopFactory::make_one_step(m_deps)->run(messages, system_prompt, tools_schema,
                                                            should_cancel, observer);
    }

    // #32：多模式目标（script/batch/watch）由专属 Agent 驱动，不是目标环可验证的
    // Verdict。若被误路由到这里（agent.active 与 agent.goal 不匹配），立即报错，
    // 避免 check_goal 返回 Failed 后仍空转 max_attempts 轮 ReAct。
    if (!has_checker(goal.type)) {
        // #32：多模式目标无 verdict checker，映射回对应的 agent.active 名便于排障
        const char* route = "script";
        switch (goal.type) {
            case AgentGoal::Batch:
                route = "batch";
                break;
            case AgentGoal::Watch:
                route = "watch";
                break;
            default:
                route = "script";
                break;
        }
        ReActResult misrouted;
        misrouted.was_error = true;
        misrouted.goal_status = GoalStatus::Failed;
        misrouted.error_message =
            "goal type has no verdict checker; route with agent.active=" + std::string(route);
        LOG_WARN("[goal_guarded] misrouted multi-mode goal type={} spec='{}'",
                 static_cast<int>(goal.type), goal_spec);
        return misrouted;
    }

    ReActResult result;  // 累积：last 轮的步骤/token + 最终 final_answer
    bool achieved = false;

    for (int attempt = 1; attempt <= goal.max_attempts; ++attempt) {
        if (should_cancel) {
            result.was_interrupted = true;
            result.error_message = "cancelled by user (goal loop)";
            LOG_WARN("[goal_guarded] cancelled, attempt={}", attempt);
            break;
        }

        // 先验：测试/文件类目标可能初始即达成，避免多余 ReAct
        Verdict pre = safe_check_goal(goal, attempt);
        if (pre.status == GoalStatus::Achieved) {
            result = ReActLoopFactory::make_one_step(m_deps)->run(  // 让 LLM 给结语
                messages, system_prompt, tools_schema, should_cancel, observer);
            result.goal_status = GoalStatus::Achieved;  // 结果回填
            achieved = true;
            LOG_INFO("[goal_guarded] achieved on pre-check, attempt={}", attempt);
            break;
        }

        // 跑一轮 ReAct（LLM 分析 + 行动）
        ReActResult step = ReActLoopFactory::make_one_step(m_deps)->run(
            messages, system_prompt, tools_schema, should_cancel, observer);
        result = step;  // 保留最后一轮（历史/token/步骤都挂到 result）
        if (step.was_interrupted || step.was_error) {
            // 流式错误/中断：不重试，向上透传错误状态。
            // P2-4：此处 break 前也要记入调用链，避免 error/interrupt 场景下
            //       tracker 只有 pre 记录而缺失终态（调用链断裂）。
            LOG_WARN("[goal_guarded] step error/interrupt, attempt={}", attempt);
            if (m_deps.tracker) {
                const std::string d = step.was_interrupted ? "interrupted before action completed"
                                      : step.error_message.empty() ? "agent step error"
                                                                   : step.error_message;
                m_deps.tracker->record_verdict(GoalStatus::Failed, d);
            }
            break;
        }

        // 观察后验证（关键：即使 LLM 主观 FinalAnswer 也算未达成）
        Verdict post = safe_check_goal(goal, attempt);
        if (post.status == GoalStatus::Achieved) {
            achieved = true;
            LOG_INFO("[goal_guarded] achieved after action, attempt={}", attempt);
            break;
        }

        // 未达成：向消息注入"继续"提示，让下一轮知道目标仍存
        messages.push_back(ChatMessage::user(std::format(
            "[goal] 上一轮行动后目标尚未达成（{}）。请继续修复/执行，直到达成", post.detail)));

        // 可选：每 N 次未达成弹 AskUser（本版由宿主通过外部中断注入，此处仅记录）
        if (goal.ask_user_every > 0 && attempt % goal.ask_user_every == 0) {
            LOG_DEBUG("[goal_guarded] ask_user checkpoint, attempt={}", attempt);
        }
    }

    if (!achieved) {
        result.goal_status = GoalStatus::Failed;
        result.was_error = true;
        if (result.error_message.empty()) {
            result.error_message =
                std::format("goal not achieved after {} attempts", goal.max_attempts);
        }
        LOG_WARN("[goal_guarded] not achieved after max_attempts={}, status={}", goal.max_attempts,
                 static_cast<int>(result.goal_status));
    } else {
        result.goal_status = GoalStatus::Achieved;
    }

    return result;
}

}  // namespace agent