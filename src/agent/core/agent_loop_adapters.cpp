#include "agent/core/agent_loop_adapters.h"

#include <format>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/agent_task_id.h"
#include "agent/core/react_step_format.h"
#include "agent/core/react_loop.h"
#include "core/events/agent_events.h"
#include "core/task/task_manager.h"
#include "liblogger/logger.h"

namespace agent {

ReActLoopAdapter::ReActLoopAdapter(std::unique_ptr<ReActLoop> loop) : m_loop(std::move(loop)) {}

AgentRunResult ReActLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    out.react = m_loop->run(*ctx.messages, ctx.system_prompt, ctx.tools_schema,
                            ctx.should_cancel ? *ctx.should_cancel : kNeverCancel(), ctx.observer);
    return out;
}

GoalGuardedLoopAdapter::GoalGuardedLoopAdapter(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

AgentRunResult GoalGuardedLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    GoalGuardedAgent agent(m_deps);
    out.react = agent.run(*ctx.messages, ctx.system_prompt, ctx.tools_schema,
                          ctx.should_cancel ? *ctx.should_cancel : kNeverCancel(), ctx.goal,
                          ctx.goal_spec, ctx.observer);
    return out;
}

const std::atomic<bool>& kNeverCancel() {
    static const std::atomic<bool> k{false};
    return k;
}

ScriptLoopAdapter::ScriptLoopAdapter(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

AgentRunResult ScriptLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    ScriptAgent agent(m_deps);
    out.react = agent.run(ctx.goal, ctx.goal_spec, *ctx.messages, ctx.observer);
    return out;
}

BatchLoopAdapter::BatchLoopAdapter(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

AgentRunResult BatchLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    BatchAgent agent(m_deps);
    out.react = agent.run(ctx.goal, ctx.goal_spec, *ctx.messages, ctx.observer);
    return out;
}

WatchLoopAdapter::WatchLoopAdapter(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

AgentRunResult WatchLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    WatchAgent agent(m_deps);
    out.react = agent.run(ctx.goal, ctx.goal_spec, *ctx.messages, ctx.observer);
    return out;
}

RoleLoopAdapter::RoleLoopAdapter(GoalAgentDeps deps, AgentType type)
    : m_deps(std::move(deps)), m_type(type) {}

AgentRunResult RoleLoopAdapter::run(AgentRunContext ctx) {
    RoleAgent agent(m_deps, role_profile_of(m_type));
    return agent.run(std::move(ctx));
}

namespace {

/// @brief 从会话消息中取最后一条用户文本来决定后台任务内容（同步、宿主锁内取值）
std::string last_user_text(const std::vector<ChatMessage>* messages) {
    if (!messages) return {};
    for (auto it = messages->rbegin(); it != messages->rend(); ++it) {
        if (it->role == ChatMessage::Role::User) {
            return it->content;
        }
    }
    return {};
}

/// @brief 后台任务保留的提示词长度上限（P2-2：避免超长请求直灌 LLM）
constexpr size_t kMaxBackgroundPromptChars = 32000;

/// @brief 后台任务不派生的工具（AgentTool）：后台不再递归起 SubAgent/后台任务，
///        防异步级联失控（P2-3 最小权限降级）。
/// @note "Agent" 需对齐 agent::tool::kAgentToolName，改动时保持同步。
constexpr const char* kBackgroundForbiddenTool = "Agent";

/// @brief 过滤后台任务不应暴露的工具 schema（输入不变，返回新数组）
nlohmann::json strip_background_forbidden_tools(const nlohmann::json& schema) {
    if (!schema.is_array()) return schema;
    nlohmann::json out = nlohmann::json::array();
    for (const auto& item : schema) {
        const bool forbidden =
            item.is_object() && item.value("name", std::string{}) == kBackgroundForbiddenTool;
        if (!forbidden) {
            out.push_back(item);
        }
    }
    return out;
}

}  // namespace

BackgroundLoopAdapter::BackgroundLoopAdapter(const GoalAgentDeps& deps) : m_deps(deps) {}

void BackgroundLoopAdapter::cancel() const noexcept {
    if (m_task_id.empty() || !m_deps.task_manager) {
        return;
    }
    // task->cancel() 仅置原子标志（协作式取消），线程安全，可在任意线程调用。
    const auto task = m_deps.task_manager->find_task(m_task_id);
    if (task) {
        task->cancel();
    }
}

AgentRunResult BackgroundLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();

    // P1-2：单次使用守卫，拒绝并发/重复 run()——m_task_id 的写入只在首调发生，
    // 彻底消除对 m_task_id 的数据竞争（UB）。
    if (m_used.exchange(true)) {
        out.react.was_error = true;
        out.react.goal_status = GoalStatus::Failed;
        out.react.error_message =
            "background agent adapter is single-use; run() called more than once";
        LOG_WARN("[background_agent] run() invoked more than once (dispatch rejected)");
        return out;
    }

    if (!m_deps.task_manager || !m_deps.provider) {
        out.react.was_error = true;
        out.react.goal_status = GoalStatus::Failed;
        out.react.error_message = "background agent requires a live task manager and LLM provider";
        LOG_WARN("[background_agent] missing task_manager/provider");
        return out;
    }
    std::string user_text = last_user_text(ctx.messages);
    if (user_text.empty()) {
        out.react.was_error = true;
        out.react.goal_status = GoalStatus::Failed;
        out.react.error_message = "background agent requires a non-empty user request";
        LOG_WARN("[background_agent] empty user request");
        return out;
    }
    if (user_text.size() > kMaxBackgroundPromptChars) {
        user_text = user_text.substr(0, kMaxBackgroundPromptChars);
        LOG_WARN("[background_agent] user request truncated to {} chars",
                 kMaxBackgroundPromptChars);
    }

    // 分发后台任务：仅捕获会话期稳定依赖的拷贝（指针均由宿主保证存活于会话周期），
    // 后台 lambda 自建独立消息缓冲，绝不引用 run() 返回后失效的 ctx.messages。
    const std::string task_id = generate_agent_task_id('b');
    m_task_id = task_id;
    out.background_task_id = task_id;

    // 拷贝需要跨 run() 存活的值语义依赖（字符串 / json），指针仅保留会话稳定的。
    GoalAgentDeps deps = m_deps;
    // 消息队列归属主会话前台循环：后台任务在独立消息缓冲中运行，不消费主会话队列
    // （否则后台/前台并发冲刷时队列可能被后台任务抢先注入，与用户预期不符）。
    deps.queue_inject_cb = nullptr;
    const std::string system_prompt = std::move(ctx.system_prompt);
    // P2-3：后台不暴露 AgentTool（防异步递归派生子 Agent/后台任务）
    const nlohmann::json tools_schema = strip_background_forbidden_tools(
        deps.registry ? deps.registry->get_all_schemas() : nlohmann::json::array());

    // 供后台子线程取消句柄：ChatSession 经 result.background_task_id 或 cancel() 定向取消
    m_deps.task_manager->launch(task_id, [deps, task_id, user_text, system_prompt,
                                          tools_schema](const std::atomic<bool>& should_cancel) {
        // 底层默认 ReAct 循环（ReActLoopFactory::make 注入会话权限/事件/压缩器）
        auto loop = ReActLoopFactory::make(deps, deps.registry, ReActLoop::Config{});
        std::vector<ChatMessage> messages;
        messages.push_back(ChatMessage::user(user_text));

        // observer 仅捕获最小依赖（P3-2）：指针 + id，避免按值拷贝大结构体；
        // 每次回调按需 find_task（P2-1），不持有对局部 task_ptr 的引用。
        auto* task_manager = deps.task_manager;
        auto* event_bus = deps.event_bus;
        ReActResult result =
            loop->run(messages, system_prompt, tools_schema, should_cancel,
                      [task_manager, event_bus, task_id](const ReActStep& step) {
                          const std::string line = format_step_line(step);
                          if (task_manager && !line.empty()) {
                              if (const auto task = task_manager->find_task(task_id)) {
                                  task->append_output(line);
                              }
                          }
                          // 进度事件：增量通知（不注入主会话 LLM 上下文），供第二层渲染
                          if (event_bus != nullptr && !line.empty()) {
                              event_bus->publish_async(BackgroundProgressEvent{
                                  .task_id = task_id,
                                  .step_number = step.step_number,
                                  .step_type = step_type_str(step.type),
                                  .content = line,
                                  .thought_text = step.thought_text,
                                  .tool_name = step.tool_name,
                                  .tool_input = step.tool_input.is_null() ? std::string{}
                                                                          : step.tool_input.dump(),
                                  .observation = step.observation,
                                  .is_error = step.is_error,
                                  .duration_ms = step.duration_ms,
                              });
                          }
                      });

        // 收尾：错误/最终答案写入输出缓冲 + 无条件发布完成事件（L-5 约定）
        std::string summary;
        if (result.was_error) {
            summary = std::format("Error: {}", result.error_message);
        } else if (!result.final_answer.empty()) {
            summary = std::format("Final: {}", result.final_answer);
        }
        if (summary.empty()) {
            summary = "(后台任务无文本输出)";
        }
        if (task_manager) {
            if (const auto task = task_manager->find_task(task_id)) {
                task->append_output(summary);
            }
        }
        if (event_bus != nullptr) {
            event_bus->publish_async(BackgroundCompletedEvent{
                .task_id = task_id,
                .final_answer = summary,
                .was_error = result.was_error,
                .duration_ms = static_cast<double>(result.total_duration_ms),
            });
        }
    });

    // 主 turn 立即返回，不阻塞主对话
    out.react.final_answer =
        std::format("后台任务已启动（task: {}）。可继续对话，进度与完成会通过事件通知。", task_id);
    return out;
}

}  // namespace agent