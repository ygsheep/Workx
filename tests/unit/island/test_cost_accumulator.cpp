/**
 * @file test_cost_accumulator.cpp
 * @brief 费用累积器单测
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include <typeindex>
#include <vector>

#include "core/events/agent_events.h"
#include "core/events/event_bus.h"
#include "core/events/stream_events.h"
#include "island/cost_accumulator.h"
#include "island/events.h"

namespace {
bool close_enough(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

using agent::AgentDoneEvent;
using agent::EventBus;
using agent::EventToken;
using agent::StreamDoneEvent;
using agent::UserInputEvent;
using island::CostAccumulator;
using island::CostSnapshot;
using island::CostUpdatedEvent;

namespace {

/// @brief 捕获 CostUpdatedEvent 的订阅助手（每个 TEST_CASE 独立实例）
struct CostCapture {
    std::vector<CostSnapshot> snaps;
    int completed_calls = 0;
};

void publish_raw_sync(const auto& ev) {
    EventBus::instance().publish_raw(std::type_index(typeid(ev)), &ev);
    EventBus::instance()
        .process_async_events();  // 同步投递 CostUpdatedEvent（publish_async 仅入队）
}

/// @brief 订阅并在 TEST_CASE 结束时自动退订，避免悬垂 lambda 导致后续用例崩溃
struct BusGuard {
    explicit BusGuard(const std::string& model = "deepseek-chat")
        : acc(EventBus::instance(), island::PricingTable::deepseek_default(), model) {
        token = EventBus::instance().subscribe<CostUpdatedEvent>(
            [this](const CostUpdatedEvent& e) { snaps.push_back(e.snapshot); });
        acc.set_on_task_completed([this] { ++completed_calls; });
    }
    ~BusGuard() { EventBus::instance().unsubscribe<CostUpdatedEvent>(token); }

    CostAccumulator acc;
    std::vector<CostSnapshot> snaps;
    int completed_calls = 0;
    EventToken token;
};

}  // namespace

TEST_CASE("cost: stream_done adds task+session and publishes event", "[island][cost]") {
    BusGuard g;

    // deepseek-chat: input 0.27 / output 1.10 / cache_read 0.07 / cache_write 0.27 (per 1M)
    StreamDoneEvent ev;
    ev.session_id = "s1";
    ev.prompt_cache_miss_tokens = 1'000'000;   // input
    ev.generated_tokens = 500'000;             // output
    ev.prompt_cache_hit_tokens = 200'000;      // cache read
    ev.cache_creation_input_tokens = 100'000;  // cache write
    publish_raw_sync(ev);

    REQUIRE(g.snaps.size() == 1);
    const auto& s = g.snaps.front();
    REQUIRE(close_enough(s.task_cost.input_usd, 0.27));
    REQUIRE(close_enough(s.task_cost.output_usd, 0.55));
    REQUIRE(close_enough(s.task_cost.cache_read_usd, 0.014));
    REQUIRE(close_enough(s.task_cost.cache_write_usd, 0.027));
    REQUIRE(close_enough(s.task_cost.total_usd, 0.27 + 0.55 + 0.014 + 0.027));
    REQUIRE(s.session_cost.total_usd == s.task_cost.total_usd);
    REQUIRE_FALSE(s.is_estimated);
    REQUIRE(s.model == "deepseek-chat");

    // 累加：再一条 stream_done，session 与 task 同步增加
    StreamDoneEvent ev2;
    ev2.session_id = "s1";
    ev2.generated_tokens = 1'000'000;
    publish_raw_sync(ev2);
    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.27 + 0.55 + 0.014 + 0.027 + 1.10));
}

TEST_CASE("cost: agent_done moves task into session and fires callback", "[island][cost]") {
    BusGuard g;

    StreamDoneEvent ev;
    ev.prompt_cache_miss_tokens = 1'000'000;
    publish_raw_sync(ev);
    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.27));

    AgentDoneEvent done;
    done.total_steps = 2;
    done.total_tool_calls = 1;
    done.total_duration_ms = 100;
    publish_raw_sync(done);

    // 任务成本归零（已并入会话），回调触发
    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.0));
    REQUIRE(close_enough(g.acc.snapshot().session_cost.total_usd, 0.27));
    REQUIRE(g.completed_calls == 1);
    REQUIRE(g.snaps.size() == 2);  // cost 更新在 done 后再次发布
    REQUIRE(close_enough(g.snaps.back().task_cost.total_usd, 0.0));
    REQUIRE(close_enough(g.snaps.back().session_cost.total_usd, 0.27));
}

TEST_CASE("cost: new user input resets task, keeps session", "[island][cost]") {
    BusGuard g;

    StreamDoneEvent ev;
    ev.prompt_cache_miss_tokens = 1'000'000;
    publish_raw_sync(ev);
    AgentDoneEvent done;
    publish_raw_sync(done);

    UserInputEvent in;
    in.text = "next question";
    in.is_local_command = false;
    publish_raw_sync(in);

    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.0));
    REQUIRE(close_enough(g.acc.snapshot().session_cost.total_usd, 0.27));
}

TEST_CASE("cost: local command events are ignored", "[island][cost]") {
    BusGuard g;

    UserInputEvent in;
    in.text = "/help";
    in.is_local_command = true;
    publish_raw_sync(in);
    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.0));

    StreamDoneEvent ev;
    ev.is_local_command = true;
    ev.generated_tokens = 1'000'000;
    publish_raw_sync(ev);
    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.0));
    REQUIRE(g.snaps.empty());
}

TEST_CASE("cost: unknown model falls back with is_estimated flag", "[island][cost]") {
    BusGuard g("gpt-4o");

    StreamDoneEvent ev;
    ev.prompt_cache_miss_tokens = 1'000'000;
    publish_raw_sync(ev);

    REQUIRE(g.snaps.size() == 1);
    REQUIRE(g.snaps.front().is_estimated);
    REQUIRE(g.snaps.front().model == "gpt-4o");
    REQUIRE(close_enough(g.snaps.front().task_cost.total_usd, 0.27));  // deepseek-chat 回退价
}

TEST_CASE("cost: zero-token stream_done publishes nothing", "[island][cost]") {
    BusGuard g;

    StreamDoneEvent ev;
    publish_raw_sync(ev);
    REQUIRE(g.snaps.empty());
    REQUIRE(close_enough(g.acc.snapshot().task_cost.total_usd, 0.0));
}
