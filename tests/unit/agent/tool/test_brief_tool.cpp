/**
 * @file test_brief_tool.cpp
 * @brief #56 方案 B：BriefTool 强制用户通信通道单元测试
 * @details 覆盖：
 *          - 输入校验（缺 question / 空 question）
 *          - fail-closed（无 event bus → Unavailable）
 *          - submitted（确认 / 自定义输入，proactive 的 approved 推断）
 *          - cancelled
 *          - timeout（宿主未响应）
 *          - 复用 AskUser 事件契约（questions 对象结构、options 对象数组）
 * @date 2026-09
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "agent/tool/BriefTool/BriefTool.h"
#include "agent/tool/context.h"
#include "core/events/agent_events.h"
#include "core/utils/error.h"
#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::tool;

namespace {

/// 填充带事件总线的 BriefTool 上下文
void fill_ctx(agent::test::MockEventBus& bus, ToolContext& ctx) {
    ctx.session_id = "test-brief";
    ctx.event_bus_ptr = &bus;
}

/// 宿主侧响应：以指定 answers / submitted 回填 promise
void respond(agent::test::MockEventBus& bus, bool submitted, std::string answer,
             const std::string& q = "去执行看看?") {
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);
    bus.subscribe<agent::AskUserRequestEvent>([=](const agent::AskUserRequestEvent& e) {
        agent::AskUserResult r;
        r.submitted = submitted;
        r.answers.emplace_back(q, std::move(answer));
        e.result_promise->set_value(std::move(r));
    });
}

}  // namespace

// ============================================================
// 输入校验
// ============================================================

TEST_CASE("BriefTool rejects missing question", "[tool][brief]") {
    agent::test::MockEventBus bus;
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"status": "proactive"})"_json, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::InvalidInput);
}

TEST_CASE("BriefTool rejects empty question", "[tool][brief]") {
    agent::test::MockEventBus bus;
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"question": ""})"_json, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::InvalidInput);
}

// ============================================================
// fail-closed
// ============================================================

TEST_CASE("BriefTool fails closed without event bus", "[tool][brief]") {
    ToolContext ctx;
    ctx.session_id = "test-brief";
    ctx.event_bus_ptr = nullptr;
    BriefTool tool;

    auto res = tool.call(R"({"question": "开工?"})"_json, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::ToolExecutionFailed);
}

// ============================================================
// submitted / 确认语义
// ============================================================

TEST_CASE("BriefTool proactive confirm yields approved", "[tool][brief]") {
    agent::test::MockEventBus bus;
    respond(bus, true, "确认");
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"question": "去执行看看?", "status": "proactive"})"_json, ctx);
    REQUIRE(res.is_ok());
    const auto& out = res.value().data;
    REQUIRE(out["status"].get<std::string>() == "submitted");
    REQUIRE(out["answer"].get<std::string>() == "确认");
    REQUIRE(out["approved"].get<bool>() == true);
}

TEST_CASE("BriefTool proactive custom answer is not auto-approved", "[tool][brief]") {
    agent::test::MockEventBus bus;
    respond(bus, true, "先别急，再想想");
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"question": "去执行看看?"})"_json, ctx);  // 默认 proactive
    REQUIRE(res.is_ok());
    const auto& out = res.value().data;
    REQUIRE(out["status"].get<std::string>() == "submitted");
    REQUIRE(out["approved"].get<bool>() == false);
}

TEST_CASE("BriefTool normal status has no approved gate", "[tool][brief]") {
    agent::test::MockEventBus bus;
    respond(bus, true, "蓝色");
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"question": "喜欢哪种配色?", "status": "normal"})"_json, ctx);
    REQUIRE(res.is_ok());
    const auto& out = res.value().data;
    REQUIRE(out["status"].get<std::string>() == "submitted");
    REQUIRE(out["answer"].get<std::string>() == "蓝色");
    // normal 模式不引入 approved 门控语义
    REQUIRE(!out.contains("approved"));
}

// ============================================================
// cancelled / timeout
// ============================================================

TEST_CASE("BriefTool cancelled maps to cancelled status", "[tool][brief]") {
    agent::test::MockEventBus bus;
    respond(bus, false, "");
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"question": "开工?"})"_json, ctx);
    REQUIRE(res.is_ok());
    const auto& out = res.value().data;
    REQUIRE(out["status"].get<std::string>() == "cancelled");
}

TEST_CASE("BriefTool times out when host never responds", "[tool][brief]") {
    agent::test::MockEventBus bus;  // 不订阅、不 flush：promise 永不 set_value
    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;

    auto res = tool.call(R"({"question": "开工?", "timeout_ms": 50})"_json, ctx);
    REQUIRE(res.is_ok());
    const auto& out = res.value().data;
    REQUIRE(out["status"].get<std::string>() == "timeout");
}

// ============================================================
// 事件契约：与 AskUser / permission_ask 一致
// ============================================================

TEST_CASE("BriefTool publishes object-format questions contract", "[tool][brief]") {
    agent::test::MockEventBus bus;
    nlohmann::json captured;
    bool got = false;
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);
    bus.subscribe<agent::AskUserRequestEvent>([&](const agent::AskUserRequestEvent& e) {
        captured = e.questions;
        got = true;
        agent::AskUserResult r;
        r.submitted = true;
        r.answers.emplace_back("去执行看看?", "确认");
        e.result_promise->set_value(std::move(r));
    });

    ToolContext ctx;
    fill_ctx(bus, ctx);
    BriefTool tool;
    REQUIRE(tool.call(R"({"question": "去执行看看?"})"_json, ctx).is_ok());

    REQUIRE(got);
    REQUIRE(captured.is_object());
    REQUIRE(captured.contains("questions"));
    REQUIRE(captured["questions"].is_array());
    REQUIRE(captured["questions"].size() == 1);
    const auto& q = captured["questions"][0];
    REQUIRE(q["question"].get<std::string>() == "去执行看看?");
    REQUIRE(q.contains("header"));
    REQUIRE(q.contains("options"));
    REQUIRE(q["options"].is_array());
    REQUIRE(q["options"].size() == 2);
    // 契约：options 必须为 {label, description} 对象数组（ftxtui handle_ask_user 解析对象选项）
    for (const auto& o : q["options"]) {
        REQUIRE(o.is_object());
        REQUIRE(o.contains("label"));
    }
}
