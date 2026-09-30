/**
 * @file test_event_bridge.cpp
 * @brief EventBridge 单元测试（B4：stop() 真正按 token 退订）
 * @details 用 MockEventBus 验证：start() 订阅全部事件 → stop() 逐个精确退订
 *          （订阅者清零、unsubscribe 被调用），不误伤其他订阅者。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "bridge/action_queue.h"
#include "bridge/event_bridge.h"
#include "core/events/agent_events.h"
#include "core/events/stream_events.h"
#include "core/events/system_events.h"
#include "helpers/mock_event_bus.h"

using namespace ftxtui;
using namespace agent::test;

namespace {

/// @brief 订阅后统计 EventBridge 管理的订阅类型数量
size_t bridge_subscriber_count(MockEventBus& bus) {
    size_t n = 0;
    n += bus.subscriber_count_typed<agent::StreamTokenEvent>();
    n += bus.subscriber_count_typed<agent::StreamDoneEvent>();
    n += bus.subscriber_count_typed<agent::StepDoneEvent>();
    n += bus.subscriber_count_typed<agent::StreamErrorEvent>();
    n += bus.subscriber_count_typed<agent::ToolCallEvent>();
    n += bus.subscriber_count_typed<agent::ToolResultEvent>();
    n += bus.subscriber_count_typed<agent::AgentDoneEvent>();
    n += bus.subscriber_count_typed<agent::AskUserRequestEvent>();
    n += bus.subscriber_count_typed<agent::AskUserTimeoutEvent>();
    n += bus.subscriber_count_typed<agent::EnterPlanModeEvent>();
    n += bus.subscriber_count_typed<agent::ExitPlanModeEvent>();
    n += bus.subscriber_count_typed<agent::CacheDiagnosticsEvent>();
    n += bus.subscriber_count_typed<agent::PlanPreviewEvent>();  // 计划模式进入前预览
    n += bus.subscriber_count_typed<agent::CompactionPausedEvent>();
    n += bus.subscriber_count_typed<agent::SubAgentProgressEvent>();
    n += bus.subscriber_count_typed<agent::SubAgentCompletedEvent>();
    n += bus.subscriber_count_typed<agent::TodoUpdatedEvent>();          // #24：待办清单更新
    n += bus.subscriber_count_typed<agent::McpStatusChangedEvent>();     // #27 M4：MCP 状态
    n += bus.subscriber_count_typed<agent::MessageQueueUpdatedEvent>();  // 消息队列更新
    n += bus.subscriber_count_typed<agent::QueuedMessagesFlushedEvent>();  // 队列冲刷回显
    n += bus.subscriber_count_typed<agent::HookProgressEvent>();  // Issue #50 M-2：Hook 进度
    n += bus.subscriber_count_typed<agent::ShutdownEvent>();
    return n;
}

}  // namespace

TEST_CASE("EventBridge start subscribes all UI events", "[event_bridge][start]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();

    REQUIRE(bridge_subscriber_count(bus) == 22);
    REQUIRE(bus.total_subscriber_count() == 22);
}

TEST_CASE("EventBridge stop unsubscribes every registered event", "[event_bridge][stop]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();

    bridge.stop();
    REQUIRE(bridge_subscriber_count(bus) == 0);
    REQUIRE(bus.total_subscriber_count() == 0);
}

TEST_CASE("EventBridge stop does not affect unrelated subscribers", "[event_bridge][stop]") {
    MockEventBus bus;
    ActionQueue queue;

    // 一个与本桥无关的订阅者（模拟 EventBus 上其他组件的订阅）
    auto unrelated_token =
        bus.subscribe<agent::StreamTokenEvent>([](const agent::StreamTokenEvent&) {});

    EventBridge bridge(bus, queue);
    bridge.start();
    bridge.stop();

    // 本桥退订不影响无关订阅者
    REQUIRE(bus.subscriber_count_typed<agent::StreamTokenEvent>() == 1);
    REQUIRE(bus.total_subscriber_count() == 1);
    bus.unsubscribe<agent::StreamTokenEvent>(unrelated_token);
}

TEST_CASE("EventBridge stop is idempotent", "[event_bridge][stop]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bridge.stop();
    // 二次 stop 安全（无异常、不重复退订造成计数错乱）
    REQUIRE_NOTHROW(bridge.stop());
    REQUIRE(bus.total_subscriber_count() == 0);
}

TEST_CASE("EventBridge dispatch maps events to actions", "[event_bridge][dispatch]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bus.set_dispatch_enabled(true);

    // 派发一个 StreamTokenEvent，应入队 TokenDelta action
    bus.publish(agent::StreamTokenEvent{
        .session_id = "s",
        .content_delta = "hi",
        .reasoning_delta = "",
        .is_thinking = false,
        .token_count = 1,
    });
    auto actions = queue.drain();
    REQUIRE(actions.size() == 1);
    const auto* delta = std::get_if<ActionTokenDelta>(&actions[0]);
    REQUIRE(delta != nullptr);
    REQUIRE(delta->content_delta == "hi");

    // 停用派发后 stop：不再入队
    bridge.stop();
    queue.drain();
    bus.publish(agent::StreamTokenEvent{
        .session_id = "s",
        .content_delta = "bye",
        .reasoning_delta = "",
        .is_thinking = false,
        .token_count = 1,
    });
    REQUIRE(queue.empty());
}

TEST_CASE("EventBridge dispatch maps McpStatusChangedEvent to ActionMcpStatus",
          "[event_bridge][dispatch][mcp]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bus.set_dispatch_enabled(true);

    bus.publish(agent::McpStatusChangedEvent{{
        {"github", "2026-07-28", 3, 1, ""},
        {"broken", "", 0, 2, "spawn failed"},
    }});
    auto actions = queue.drain();
    REQUIRE(actions.size() == 1);
    const auto* status = std::get_if<ActionMcpStatus>(&actions[0]);
    REQUIRE(status != nullptr);
    REQUIRE(status->servers.size() == 2);
    REQUIRE(status->servers[0].name == "github");
    REQUIRE(status->servers[0].state == 1);
    REQUIRE(status->servers[1].name == "broken");
    REQUIRE(status->servers[1].state == 2);
    REQUIRE(status->servers[1].error == "spawn failed");
}

TEST_CASE("EventBridge dispatch maps QueuedMessagesFlushedEvent to user echo + busy",
          "[event_bridge][dispatch][queue]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bus.set_dispatch_enabled(true);

    const std::string merged = "[排队消息 1/1]\n你好\n━━━━━━━━━━";
    bus.publish(agent::QueuedMessagesFlushedEvent{
        .session_id = "s",
        .merged_text = merged,
    });
    auto actions = queue.drain();
    REQUIRE(actions.size() == 2);
    // 先回显 user 消息，再置 busy=true（为新一轮流式回复建立上下文）
    const auto* append = std::get_if<ActionAppendMessage>(&actions[0]);
    REQUIRE(append != nullptr);
    REQUIRE(append->role == "user");
    REQUIRE(append->text == merged);
    const auto* busy = std::get_if<ActionSetBusy>(&actions[1]);
    REQUIRE(busy != nullptr);
    REQUIRE(busy->busy == true);
}

TEST_CASE("EventBridge plan events map to ActionSetMode", "[event_bridge][dispatch][mode]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bus.set_dispatch_enabled(true);

    // 进入计划模式（AI 工具 EnterPlanMode）→ 模式位切到 plan
    bus.publish(agent::EnterPlanModeEvent{.session_id = "s", .reason = "research"});
    auto actions = queue.drain();
    REQUIRE(actions.size() == 1);
    const auto* enter = std::get_if<ActionSetMode>(&actions[0]);
    REQUIRE(enter != nullptr);
    REQUIRE(enter->label == "plan");

    // 退出计划模式（ExitPlanModeV2）→ 模式位回到标准
    bus.publish(agent::ExitPlanModeEvent{.session_id = "s", .plan = "", .approved = true});
    actions = queue.drain();
    REQUIRE(actions.size() == 1);
    const auto* exit = std::get_if<ActionSetMode>(&actions[0]);
    REQUIRE(exit != nullptr);
    REQUIRE(exit->label == "standard");
}