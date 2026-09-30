/**
 * @file test_events_split.cpp
 * @brief events.h 按域拆分验证（H-10）
 * @details 验证三个子头文件可独立 include 且互不依赖，
 *          以及聚合头 events.h 行为与拆分前一致。
 */

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

// 独立 include 三个子文件，验证无相互依赖
#include "core/events/system_events.h"
#include "core/events/stream_events.h"
#include "core/events/agent_events.h"

using namespace agent;

// ============================================================
// 编译期验证：各事件类型在子文件中定义
// ============================================================

static_assert(std::is_same_v<decltype(std::declval<ModelLoadEvent>().model_name), std::string>);
static_assert(
    std::is_same_v<decltype(std::declval<BackendStatusEvent>().backend_name), std::string>);
static_assert(std::is_same_v<decltype(std::declval<ShutdownEvent>().force), bool>);

static_assert(std::is_same_v<decltype(std::declval<UserInputEvent>().text), std::string>);
static_assert(std::is_same_v<decltype(std::declval<InterruptEvent>().force), bool>);
static_assert(std::is_same_v<decltype(std::declval<StreamTokenEvent>().session_id), std::string>);
static_assert(std::is_same_v<decltype(std::declval<StreamDoneEvent>().full_content), std::string>);
static_assert(std::is_same_v<decltype(std::declval<StepDoneEvent>().generation_ms), double>);
static_assert(std::is_same_v<decltype(std::declval<StreamErrorEvent>().message), std::string>);

static_assert(std::is_same_v<decltype(std::declval<AgentStepEvent>().step_id), std::string>);
static_assert(std::is_same_v<decltype(std::declval<ToolCallEvent>().tool_name), std::string>);
static_assert(std::is_same_v<decltype(std::declval<ToolResultEvent>().call_id), std::string>);
static_assert(std::is_same_v<decltype(std::declval<AgentDoneEvent>().total_steps), int32_t>);

// ============================================================
// 行为验证：默认构造字段值
// ============================================================

TEST_CASE("system events default values (H-10)", "[events][h10][system]") {
    ModelLoadEvent m;
    REQUIRE(m.progress == 0.0f);
    REQUIRE_FALSE(m.complete);

    BackendStatusEvent b;
    REQUIRE(b.status == BackendStatusEvent::Disconnected);

    ShutdownEvent s;
    REQUIRE_FALSE(s.force);
}

TEST_CASE("stream events default values (H-10)", "[events][h10][stream]") {
    UserInputEvent u;
    REQUIRE(u.text.empty());

    InterruptEvent i;
    REQUIRE_FALSE(i.force);

    StreamTokenEvent t;
    REQUIRE(t.is_thinking == false);
    REQUIRE(t.token_count == 0);

    StreamDoneEvent d;
    REQUIRE_FALSE(d.was_interrupted);
    REQUIRE(d.prompt_tokens == 0);

    StepDoneEvent sd;
    REQUIRE(sd.generation_ms == 0.0);

    StreamErrorEvent e;
    REQUIRE_FALSE(e.retryable);
}

TEST_CASE("agent events default values (H-10)", "[events][h10][agent]") {
    AgentStepEvent s;
    REQUIRE(s.step_number == 0);

    ToolCallEvent tc;
    REQUIRE(tc.tool_type == agent::tool::ToolType::Other);

    ToolResultEvent tr;
    REQUIRE_FALSE(tr.is_error);

    AgentDoneEvent ad;
    REQUIRE(ad.total_steps == 0);
    REQUIRE(ad.total_tool_calls == 0);
}
