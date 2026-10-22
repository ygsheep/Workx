/**
 * @file test_island_event_bridge.cpp
 * @brief 事件桥单测：总线事件 → IslandServer 推送
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <typeindex>

#include "core/events/agent_events.h"
#include "core/events/event_bus.h"
#include "core/events/stream_events.h"
#include "island/events.h"
#include "island/island_event_bridge.h"
#include "island/island_server.h"

using agent::AgentDoneEvent;
using agent::EventBus;
using agent::StreamDoneEvent;
using agent::ToolCallEvent;
using agent::ToolResultEvent;
using agent::UserInputEvent;
using island::IslandEventBridge;
using island::IslandServer;
using island::IslandServerConfig;

namespace {

void publish_raw_sync(const auto& ev) {
    EventBus::instance().publish_raw(std::type_index(typeid(ev)), &ev);
}

std::string unique_endpoint() {
#ifdef _WIN32
    return "\\\\.\\pipe\\workx-island-bridge-test" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
#else
    return "/tmp/workx-island-bridge-test-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

}  // namespace

TEST_CASE("bridge: user input emits task_started (local command skipped)", "[island][bridge]") {
    auto& bus = EventBus::instance();
    IslandServerConfig cfg;
    cfg.endpoint = unique_endpoint();
    IslandServer server(std::move(cfg));
    server.start();
    IslandEventBridge bridge(bus, server);

    const size_t before = server.ring_size();
    UserInputEvent ev;
    ev.text = "hello";
    publish_raw_sync(ev);
    REQUIRE(server.ring_size() == before + 1);

    UserInputEvent local;
    local.text = "/help";
    local.is_local_command = true;
    publish_raw_sync(local);
    REQUIRE(server.ring_size() == before + 1);  // 本地命令不推送

    bridge.unsubscribe_all();
    server.stop();
}

TEST_CASE("bridge: tool call caches name, result erases it", "[island][bridge]") {
    auto& bus = EventBus::instance();
    IslandServerConfig cfg;
    cfg.endpoint = unique_endpoint();
    IslandServer server(std::move(cfg));
    server.start();
    IslandEventBridge bridge(bus, server);

    ToolCallEvent tc;
    tc.call_id = "call_42";
    tc.tool_name = "Bash";
    publish_raw_sync(tc);
    REQUIRE(bridge.tool_name_cache_size() == 1);

    ToolResultEvent tr;
    tr.call_id = "call_42";
    tr.result = "ok";
    tr.is_error = false;
    publish_raw_sync(tr);
    REQUIRE(bridge.tool_name_cache_size() == 0);

    bridge.unsubscribe_all();
    server.stop();
}

TEST_CASE("bridge: stream done emits llm_done with token counters", "[island][bridge]") {
    auto& bus = EventBus::instance();
    IslandServerConfig cfg;
    cfg.endpoint = unique_endpoint();
    IslandServer server(std::move(cfg));
    server.start();
    IslandEventBridge bridge(bus, server);

    const size_t before = server.ring_size();
    StreamDoneEvent ev;
    ev.prompt_cache_miss_tokens = 100;
    ev.prompt_cache_hit_tokens = 200;
    ev.cache_creation_input_tokens = 300;
    ev.generated_tokens = 400;
    publish_raw_sync(ev);
    REQUIRE(server.ring_size() == before + 1);

    bridge.unsubscribe_all();
    server.stop();
}

TEST_CASE("bridge: agent done and error map to events", "[island][bridge]") {
    auto& bus = EventBus::instance();
    IslandServerConfig cfg;
    cfg.endpoint = unique_endpoint();
    IslandServer server(std::move(cfg));
    server.start();
    IslandEventBridge bridge(bus, server);

    const size_t before = server.ring_size();
    AgentDoneEvent done;
    done.total_steps = 3;
    publish_raw_sync(done);
    REQUIRE(server.ring_size() == before + 1);

    agent::StreamErrorEvent err;
    err.message = "boom";
    publish_raw_sync(err);
    REQUIRE(server.ring_size() == before + 2);

    bridge.unsubscribe_all();
    server.stop();
}

TEST_CASE("bridge: unsubscribe_all detaches from bus", "[island][bridge]") {
    auto& bus = EventBus::instance();
    IslandServerConfig cfg;
    cfg.endpoint = unique_endpoint();
    IslandServer server(std::move(cfg));
    server.start();
    IslandEventBridge bridge(bus, server);

    bridge.unsubscribe_all();
    const size_t before = server.ring_size();
    UserInputEvent ev;
    ev.text = "after unsubscribe";
    publish_raw_sync(ev);
    REQUIRE(server.ring_size() == before);  // 不再收到

    server.stop();
}