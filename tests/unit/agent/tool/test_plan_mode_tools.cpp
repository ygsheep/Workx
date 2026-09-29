/**
 * @file test_plan_mode_tools.cpp
 * @brief #28 EnterPlanMode/ExitPlanModeV2 工具单元测试
 * @details 覆盖：
 *          - EnterPlanModeTool：模式回调切换 Plan、发布 EnterPlanModeEvent、无回调容错
 *          - ExitPlanModeV2Tool：参数校验、批准路径（回调回 Default + 事件 approved）、
 *            拒绝路径（保持 Plan）、fail-closed（无 event_bus）、Bypass 不降级
 */

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "agent/tool/PlanMode/enter_plan_mode_tool.h"
#include "agent/tool/PlanMode/exit_plan_mode_v2_tool.h"
#include "agent/tool/context.h"
#include "core/config/config_manager.h"
#include "core/events/agent_events.h"
#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::test;

namespace {

void fill_ctx(ToolContext& ctx, agent::test::MockEventBus& bus) {
    ctx.cwd = ".";
    ctx.session_id = "test-session";
    ctx.config_manager_ptr = &ConfigManager::instance();
    ctx.event_bus_ptr = &bus;
}

/// 订阅 AskUserRequestEvent 并自动回填结果（模拟 TUI 确认面板）
/// @details 需要 dispatch_enabled + async_auto_flush：工具 call 阻塞等待
///          result_promise 时，发布即同步派发到订阅回调并回填。
void auto_answer(agent::test::MockEventBus& bus, bool yes) {
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);
    bus.subscribe<AskUserRequestEvent>([yes](const AskUserRequestEvent& e) {
        AskUserResult result;
        result.submitted = true;
        result.answers.emplace_back("permission", yes ? "Yes" : "No");
        e.result_promise->set_value(result);
    });
}

}  // namespace

// ============================================================
// EnterPlanModeTool
// ============================================================

TEST_CASE("EnterPlanModeTool switches mode to Plan via callback", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);
    PermissionMode captured = PermissionMode::Default;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode m) {
        callback_called = true;
        captured = m;
    };

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "refactor"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(callback_called);
    REQUIRE(captured == PermissionMode::Plan);
}

TEST_CASE("EnterPlanModeTool publishes EnterPlanModeEvent", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "refactor"})"_json, ctx);
    REQUIRE(res.is_ok());
    bus.process_async_events();
    REQUIRE(bus.published_count<EnterPlanModeEvent>() == 1);
}

TEST_CASE("EnterPlanModeTool tolerant without callback", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.on_permission_mode_changed = nullptr;  // 未接线

    EnterPlanModeTool tool;
    auto res = tool.call(R"({})"_json, ctx);
    REQUIRE(res.is_ok());
}

// ============================================================
// ExitPlanModeV2Tool
// ============================================================

TEST_CASE("ExitPlanModeV2Tool requires plan field", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({})"_json, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::MissingArgument);
}

TEST_CASE("ExitPlanModeV2Tool approved restores Default mode", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;
    PermissionMode captured = PermissionMode::Plan;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode m) {
        callback_called = true;
        captured = m;
    };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "refactor src/x.cpp"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(callback_called);
    REQUIRE(captured == PermissionMode::Default);

    bus.process_async_events();
    REQUIRE(bus.published_count<ExitPlanModeEvent>() == 1);
}

TEST_CASE("ExitPlanModeV2Tool approved event carries plan and approval", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;

    std::string captured_plan;
    bool captured_approved = false;
    bus.subscribe<ExitPlanModeEvent>([&](const ExitPlanModeEvent& e) {
        captured_plan = e.plan;
        captured_approved = e.approved;
    });

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    bus.process_async_events();
    REQUIRE(captured_plan == "plan-text");
    REQUIRE(captured_approved);
}

TEST_CASE("ExitPlanModeV2Tool declined stays in plan mode", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, false);
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode) { callback_called = true; };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE_FALSE(callback_called);  // 保持 Plan

    bus.process_async_events();
    REQUIRE(bus.published_count<ExitPlanModeEvent>() == 1);
}

TEST_CASE("ExitPlanModeV2Tool fails closed without event bus", "[tool][plan_mode]") {
    ToolContext ctx;
    ctx.cwd = ".";
    ctx.config_manager_ptr = &ConfigManager::instance();
    ctx.event_bus_ptr = nullptr;  // 无确认通道
    ctx.permission_mode = PermissionMode::Plan;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode) { callback_called = true; };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE_FALSE(callback_called);  // fail-closed：未批准，不退出
    REQUIRE(res.value().text.find("not approved") != std::string::npos);
}

TEST_CASE("ExitPlanModeV2Tool does not downgrade bypass mode", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::BypassPermissions;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode) { callback_called = true; };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE_FALSE(callback_called);  // Bypass 不降级
}

// ============================================================
// #28 评审 #1/#3：Bypass 禁止降级 + 幂等 + 恢复原模式
// ============================================================

TEST_CASE("EnterPlanModeTool rejects entry when host forbids (bypass)",
          "[tool][plan_mode][review]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::BypassPermissions;
    bool host_called = false;
    ctx.on_enter_plan_mode = [&]() -> bool {
        host_called = true;
        return false;  // 宿主判定 Bypass 禁止降级
    };

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "refactor"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(host_called);
    bus.process_async_events();
    REQUIRE(bus.published_count<EnterPlanModeEvent>() == 0);  // 未进入，不发布
    REQUIRE(res.value().text.find("not entered") != std::string::npos);
}

TEST_CASE("EnterPlanModeTool is idempotent when already in plan mode",
          "[tool][plan_mode][review]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;
    bool host_called = false;
    ctx.on_enter_plan_mode = [&]() -> bool {
        host_called = true;
        return false;  // 宿主判定已在 Plan，幂等
    };

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "again"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(host_called);
    bus.process_async_events();
    REQUIRE(bus.published_count<EnterPlanModeEvent>() == 0);
    REQUIRE(res.value().text.find("Already in plan mode") != std::string::npos);
}

TEST_CASE("EnterPlanModeTool enters via host callback and publishes event",
          "[tool][plan_mode][review]") {
    MockEventBus bus;
    ToolContext ctx;
    fill_ctx(ctx, bus);
    bool host_called = false;
    ctx.on_enter_plan_mode = [&]() -> bool {
        host_called = true;
        return true;  // 宿主允许进入
    };

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "refactor"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(host_called);
    bus.process_async_events();
    REQUIRE(bus.published_count<EnterPlanModeEvent>() == 1);
    REQUIRE(res.value().text.find("Entered plan mode") != std::string::npos);
}

TEST_CASE("ExitPlanModeV2Tool approved restores pre-plan mode via host callback",
          "[tool][plan_mode][review]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx;
    fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;
    bool host_called = false;
    ctx.on_exit_plan_mode = [&]() { host_called = true; };  // 宿主恢复原模式（如 AcceptEdits）

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(host_called);  // 走宿主恢复原模式，而非硬编码 Default
}
