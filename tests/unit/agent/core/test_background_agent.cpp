/**
 * @file test_background_agent.cpp
 * @brief BackgroundAgent（长时运行、不阻塞主对话、事件通知）测试
 * @details 验证：
 *          - agent.active="background"/"bg" 解析到 AgentType::Background
 *          - BackgroundLoopAdapter 分发后立即返回 task_id（Mock 不执行任务函数）
 *          - 返回结果携带 background_task_id（'b' 前缀）与启动提示，无阻塞、无错误
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <vector>

#include "agent/api/chat_types.h"
#include "agent/core/agent_loop_adapters.h"
#include "agent/core/agent_type.h"
#include "agent/core/goal_guarded_agent.h"  // GoalAgentDeps
#include "agent/core/i_agent_loop.h"
#include "core/events/event_bus.h"   // EventBus::instance()
#include "core/task/task_manager.h"  // ITaskManager / TaskType
#include "helpers/mock_provider.h"
#include "helpers/mock_task_manager.h"

using namespace agent;
using namespace agent::test;

// MockTaskManager::launch 只记录不执行任务函数，故不会真正跑底层 ReActLoop，
// 仅验证"分发后立即返回 + task_id"。真实异步执行由 ChatSession 集成路径覆盖。
TEST_CASE("background: 分发后立即返回 task_id（不阻塞主对话）", "[agent][background]") {
    MockTaskManager tm;
    MockCompletionProvider provider;
    auto registry = std::make_shared<tool::ToolRegistry>();

    GoalAgentDeps deps;
    deps.provider = &provider;
    deps.registry = registry;
    deps.task_manager = &tm;
    deps.event_bus = &EventBus::instance();  // 生命周期事件总线
    deps.cwd = ".";
    deps.session_id = "sess-1";

    BackgroundLoopAdapter adapter(deps);
    REQUIRE(adapter.type() == AgentType::Background);

    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user("请分析一下日志里的报错并给出修复方案"));

    AgentRunContext ctx;
    ctx.messages = &messages;
    ctx.system_prompt = "You are a helpful assistant.";

    AgentRunResult out = adapter.run(ctx);

    // 主 turn 成功返回，无错误
    REQUIRE_FALSE(out.react.was_error);
    REQUIRE(out.agent_type == AgentType::Background);

    // 已分发一个后台任务，且返回 task_id 为 'b' 前缀 + 8 随机
    REQUIRE(tm.launched_count() == 1);
    REQUIRE_FALSE(out.background_task_id.empty());
    REQUIRE(out.background_task_id.size() == 9);
    REQUIRE(out.background_task_id.front() == 'b');
    REQUIRE(adapter.task_id() == out.background_task_id);

    // 返回提示含 task_id，主 turn 可继续对话
    REQUIRE(out.react.final_answer.find(out.background_task_id) != std::string::npos);
    REQUIRE(out.react.final_answer.find("后台任务已启动") != std::string::npos);

    // 主会话消息未被后台任务改写（run 立即返回，未触碰 ctx.messages）
    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0].role == ChatMessage::Role::User);
}

TEST_CASE("background: 缺少 task_manager/provider 时返回错误而非分发", "[agent][background]") {
    BackgroundLoopAdapter adapter(GoalAgentDeps{});
    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user("hi"));

    AgentRunContext ctx;
    ctx.messages = &messages;

    AgentRunResult out = adapter.run(ctx);
    REQUIRE(out.react.was_error);
    REQUIRE(out.background_task_id.empty());
}

TEST_CASE("background: 单次使用守卫 — 二次 run 被拒绝（P1-2）", "[agent][background]") {
    MockTaskManager tm;
    MockCompletionProvider provider;
    auto registry = std::make_shared<tool::ToolRegistry>();

    GoalAgentDeps deps;
    deps.provider = &provider;
    deps.registry = registry;
    deps.task_manager = &tm;
    deps.cwd = ".";
    deps.session_id = "sess-1";

    BackgroundLoopAdapter adapter(deps);
    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user("first"));

    AgentRunContext ctx;
    ctx.messages = &messages;
    ctx.system_prompt = "sys";

    REQUIRE_FALSE(adapter.run(ctx).react.was_error);  // 首次成功（分发）
    const AgentRunResult second = adapter.run(ctx);   // 二次应被单次守卫拒绝
    REQUIRE(second.react.was_error);
    REQUIRE(second.background_task_id.empty());
    REQUIRE(second.react.error_message.find("single-use") != std::string::npos);
    REQUIRE(tm.launched_count() == 1);  // 未重复分发
}

TEST_CASE("background: cancel() 定向取消已分发任务（P1-1）", "[agent][background]") {
    MockTaskManager tm;
    MockCompletionProvider provider;
    auto registry = std::make_shared<tool::ToolRegistry>();

    GoalAgentDeps deps;
    deps.provider = &provider;
    deps.registry = registry;
    deps.task_manager = &tm;
    deps.cwd = ".";
    deps.session_id = "sess-1";

    BackgroundLoopAdapter adapter(deps);
    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user("task"));

    AgentRunContext ctx;
    ctx.messages = &messages;
    ctx.system_prompt = "sys";

    const AgentRunResult out = adapter.run(ctx);
    REQUIRE_FALSE(out.react.was_error);
    REQUIRE_FALSE(out.background_task_id.empty());

    adapter.cancel();
    const auto task = tm.find_task(out.background_task_id);
    REQUIRE(task != nullptr);
    REQUIRE(task->shouldCancel());  // 协作式取消标志已置位

    // task_id 按值返回（header API），不暴露内部缓冲引用
    REQUIRE(adapter.task_id() == out.background_task_id);
}