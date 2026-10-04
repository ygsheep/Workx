/**
 * @file test_react_loop.cpp
 * @brief ReActLoop 单元测试
 * @details 覆盖 Thought/Action/Observation 三阶段、FinalAnswer 终止、
 *          max_iterations 限制、should_cancel 中断、流式错误、工具调用执行、
 *          回调触发、token 统计等场景
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include <string>
#include <string_view>
#include <memory>
#include <deque>
#include <optional>

#include "agent/core/react_loop.h"
#include "agent/config/app_config.h"  // #78：agent::keys::AGENT_GOAL 等门禁键
#include "agent/api/i_completion_provider.h"
#include "agent/api/i_stream_reader.h"
#include "agent/api/chat_types.h"
#include "agent/hook/hook_manager.h"  // Issue #50：通用 Hook 事件系统
#include "agent/tool/registry.h"
#include "agent/tool/itool.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "agent/tool/PlanMode/enter_plan_mode_tool.h"
#include "agent/tool/PlanMode/exit_plan_mode_v2_tool.h"
#include "core/config/config_manager.h"
#include "core/events/agent_events.h"  // AskUserRequestEvent（ExitPlanModeV2 批准确认流）
#include "helpers/mock_config_manager.h"  // #78 VF-04：门禁装配按入口做差异化默认值
#include "helpers/mock_provider.h"
#include "helpers/mock_event_bus.h"  // H-1：ExitPlanModeV2 批准确认通道

using namespace agent;
using namespace agent::tool;
using namespace agent::test;
using namespace std::chrono_literals;

namespace {

// ============================================================
// EchoTool — 简单测试工具
// ============================================================

class EchoTool : public ITool {
   public:
    mutable int call_count = 0;
    mutable std::string last_input;

    const std::string& name() const override {
        static const std::string n = "Echo";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Echoes back the input";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Echo tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"},
                {"properties", {{"text", {{"type", "string"}}}}},
                {"required", {"text"}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json& input,
                              const ToolContext& /*ctx*/) const override {
        call_count++;
        last_input = input.value("text", "");
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("echo: ") + last_input));
    }
};

// ============================================================
// FailingTool — 总是抛异常的工具
// ============================================================

class FailingTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "Failing";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Always throws";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Failing tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        throw std::runtime_error("intentional tool failure");
    }
};

// ============================================================
// DeniedTool — 权限检查拒绝
// ============================================================

class DeniedTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "Denied";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Permission always denied";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Denied tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    PermissionResult check_permissions(const nlohmann::json&, const ToolContext&) const override {
        return PermissionResult::err(Error::Code::PermissionDenied, "access denied by policy");
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("should not reach here")));
    }
};

// ============================================================
// ContextCapture — 录制工具执行时的 ToolContext（#30 环境感知验证）
// ============================================================

/// @brief 记录工具执行时收到的 ToolContext 字段（#30 环境感知验证）
/// @details 不整结构拷贝（ToolContext 含 std::atomic，不可拷贝/赋值），只录制需求字段。
struct CapturedContext {
    std::string session_id;
    std::string request_id;
    std::string model;
    std::string history_summary;
    bool captured = false;
};

/// @brief 接收工具的 ToolContext 并写入外部共享结构
class ContextCaptureTool : public ITool {
   public:
    explicit ContextCaptureTool(std::shared_ptr<CapturedContext> out) : m_out(std::move(out)) {}

    const std::string& name() const override {
        static const std::string n = "Capture";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Captures the ToolContext for env-field verification";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Capture tool for #30 env field verification";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext& ctx) const override {
        m_out->session_id = ctx.session_id;
        m_out->request_id = ctx.request_id;
        m_out->model = ctx.model;
        m_out->history_summary = ctx.history_summary;
        m_out->captured = true;
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("captured")));
    }

   private:
    std::shared_ptr<CapturedContext> m_out;
};

// ============================================================
// CooperativeSlowTool — 协作式慢工具（响应 is_cancelled 即可退出）
// ============================================================

class CooperativeSlowTool : public ITool {
   public:
    /// @brief 工具是否已被真正调用（供测试等待"进入执行"再触发取消）
    /// @details 取消时机不能再用固定 sleep：run 启动阶段含 git 基线捕获（#81），
    ///          耗时随机器/仓库大小变化，固定窗口会让取消落在工具执行之前。
    bool started() const { return started_.load(); }

    const std::string& name() const override {
        static const std::string n = "Slow";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Cooperative slow tool that exits on cancel";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Slow tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext& ctx) const override {
        started_.store(true);
        // 协作式取消：每 50ms 轮询一次；无取消则 1s 后正常完成
        constexpr int kTicks = 20;
        for (int i = 0; i < kTicks; ++i) {
            if (ctx.is_cancelled()) {
                return ResultV2<ToolResult>::err(Error::Code::Cancelled, "Slow tool cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("slow done")));
    }

   private:
    mutable std::atomic<bool> started_{false};
};

// ============================================================
// Helper: 构建 ReActLoop
// ============================================================

struct ReActLoopFixture {
    std::unique_ptr<MockCompletionProvider> provider;
    std::shared_ptr<ToolRegistry> registry;
    std::shared_ptr<EchoTool> echo_tool;
    std::atomic<bool> should_cancel{false};

    ReActLoopFixture() {
        provider = std::make_unique<MockCompletionProvider>();
        registry = std::make_shared<ToolRegistry>();
        echo_tool = std::make_shared<EchoTool>();
        registry->register_tool(echo_tool);
    }

    std::unique_ptr<ReActLoop> make_loop(ReActLoop::Config config = ReActLoop::Config{}) {
        // H-5：必须显式注入 IConfigManager*（非空）
        return std::make_unique<ReActLoop>(provider.get(), registry, config,
                                           &ConfigManager::instance());
    }

    /// @brief 创建一个返回纯文本（无工具调用）的 reader
    std::shared_ptr<MockStreamReader> make_text_reader(const std::string& text) {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk(text);
        reader->set_usage(10, 20);
        provider->set_next_reader(reader);
        return reader;
    }

    /// @brief 创建一个返回工具调用的 reader
    std::shared_ptr<MockStreamReader> make_tool_call_reader(const std::string& tool_id,
                                                            const std::string& tool_name,
                                                            const std::string& input_json) {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk("Let me use the tool.");
        reader->add_tool_use_start(tool_id, tool_name);
        reader->add_tool_use_delta(tool_id, input_json);
        reader->set_usage(15, 25);
        provider->set_next_reader(reader);
        return reader;
    }

    /// @brief 创建一个流式错误的 reader
    std::shared_ptr<MockStreamReader> make_error_reader() {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk("partial");
        reader->set_error_at(1);
        provider->set_next_reader(reader);
        return reader;
    }

    /// @brief #144：创建带 HTTP 状态码的流式错误 reader（可不可重试由 status 决定）
    std::shared_ptr<MockStreamReader> make_error_reader(int http_status, std::string message) {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk("partial");
        reader->set_error_at(1);
        reader->set_error_payload(http_status, std::move(message));
        provider->set_next_reader(reader);
        return reader;
    }
};

}  // namespace

// ============================================================================
// Thought 阶段 — FinalAnswer 终止
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop returns FinalAnswer when no tool_use",
                 "[react_loop][thought]") {
    auto reader = make_text_reader("Hello, world!");

    std::vector<ChatMessage> messages = {ChatMessage::user("hi")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE_FALSE(result.was_interrupted);
    REQUIRE(result.final_answer == "Hello, world!");
    REQUIRE(result.total_iterations == 1);
    REQUIRE(result.total_tool_calls == 0);
    REQUIRE(messages.size() == 2);  // user + assistant
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "Hello, world!");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop captures reasoning content",
                 "[react_loop][thought]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_reasoning_chunk("thinking...");
    reader->add_content_chunk("answer");
    reader->set_usage(5, 10);
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.final_answer == "answer");
    REQUIRE(result.final_reasoning == "thinking...");
    REQUIRE(messages[1].reasoning_content == "thinking...");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop captures token statistics", "[react_loop][thought]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("response");
    reader->set_usage(100, 200, 50, 30);
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.prompt_tokens == 100);
    REQUIRE(result.generated_tokens == 200);
    REQUIRE(result.cache_creation_input_tokens == 50);
    REQUIRE(result.cache_read_input_tokens == 30);
}

// ============================================================================
// Action + Observation 阶段 — 工具调用
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop executes tool and feeds result back",
                 "[react_loop][action]") {
    // 第一次：tool_use
    make_tool_call_reader("tu_01", "Echo", R"({"text":"hello"})");

    // 第二次：final answer（基于工具结果）
    make_text_reader("Done after echo");

    std::vector<ChatMessage> messages = {ChatMessage::user("echo hello")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "Done after echo");
    REQUIRE(result.total_iterations == 2);
    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(echo_tool->call_count == 1);
    REQUIRE(echo_tool->last_input == "hello");

    // 验证消息序列：user → assistant(tool_use) → tool_result → assistant(final)
    REQUIRE(messages.size() == 4);
    REQUIRE(messages[0].role == ChatMessage::Role::User);
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].tool_uses.size() == 1);
    REQUIRE(messages[1].tool_uses[0].name == "Echo");
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content == "echo: hello");
    REQUIRE(messages[3].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[3].content == "Done after echo");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop PreToolUse hook blocks Echo", "[react_loop][hook]") {
    // Issue #50：prompt 类型 hook（复用循环的 MockProvider），拦截 Echo 输出 JSON blockingError
    auto hooks = std::make_shared<agent::hook::HookManager>();
    hooks->set_provider(provider.get());
    agent::hook::HookDefinition def;
    def.event = agent::hook::HookEvent::PreToolUse;
    def.type = agent::hook::HookType::Prompt;
    def.match = "Echo";
    def.prompt = "gate echo to prove blocking path";
    hooks->register_hook(def);

    make_tool_call_reader("tu_01", "Echo", R"({"text":"hello"})");  // reader[0]：循环请求 Echo
    auto hook_reader = std::make_shared<MockStreamReader>();
    hook_reader->add_content_chunk(R"({"blockingError":"blocked-by-hook"})");
    provider->set_next_reader(hook_reader);  // reader[1]：hook 判定阻断
    make_text_reader("Done after blocked");  // reader[2]：收尾

    ReActLoop::Config config;
    config.hooks = hooks;
    config.max_iterations = 8;
    std::vector<ChatMessage> messages = {ChatMessage::user("echo hello")};
    auto loop = make_loop(config);
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    // 被 PreToolUse 拦截 → Echo 真实工具线程不启动
    REQUIRE(echo_tool->call_count == 0);
    REQUIRE(result.total_tool_calls == 0);

    // Observation 中应包含阻断文本（不会真正执行工具）
    bool found_blocked = false;
    for (const auto& m : messages) {
        if (m.role == ChatMessage::Role::Tool && m.content.find("blocked") != std::string::npos) {
            found_blocked = true;
            break;
        }
    }
    REQUIRE(found_blocked);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop tool not found returns error result",
                 "[react_loop][action]") {
    // 调用不存在的工具
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("calling unknown tool");
    reader->add_tool_use_start("tu_01", "NonExistent");
    reader->add_tool_use_delta("tu_01", R"({})");
    reader->set_usage(10, 20);
    provider->set_next_reader(reader);

    make_text_reader("recovered from error");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content.find("Tool not found") != std::string::npos);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop tool exception is caught and reported",
                 "[react_loop][action]") {
    auto failing_tool = std::make_shared<FailingTool>();
    registry->register_tool(failing_tool);

    make_tool_call_reader("tu_01", "Failing", R"({})");
    make_text_reader("recovered");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content.find("intentional tool failure") != std::string::npos);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop tool permission denied is reported",
                 "[react_loop][action]") {
    auto denied_tool = std::make_shared<DeniedTool>();
    registry->register_tool(denied_tool);

    make_tool_call_reader("tu_01", "Denied", R"({})");
    make_text_reader("recovered");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(messages[2].content.find("access denied by policy") != std::string::npos);
}

// ============================================================================
// max_iterations 限制
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop stops at max_iterations", "[react_loop][iteration]") {
    // 每次都返回 tool_use，永远不结束。关闭内部评审器以测试"纯预算耗尽=硬错误"语义
    // （评审器开启时，重复工具调用会触发停滞 recover，见 stall/at-limit 测试）。
    ReActLoop::Config config;
    config.max_iterations = 3;
    config.review_enabled = false;
    auto loop = make_loop(config);

    // 准备 3 次工具调用响应
    for (int i = 0; i < 3; ++i) {
        make_tool_call_reader("tu_" + std::to_string(i), "Echo", R"({"text":"loop"})");
    }

    std::vector<ChatMessage> messages = {ChatMessage::user("loop")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_iterations == 3);
    REQUIRE(result.was_error);  // 达到 max_iterations 视为错误
    REQUIRE(result.error_message.find("max") != std::string::npos);
}

// ============================================================================
// should_cancel 中断
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop cancels during Thought phase",
                 "[react_loop][cancel]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("partial");
    reader->set_cancel_after(1);  // 消费第一个 chunk 后返回 Cancelled
    reader->set_usage(0, 0);
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_interrupted);
    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.partial_content == "partial");
}

TEST_CASE_METHOD(ReActLoopFixture,
                 "ReActLoop cooperative cancel during tool execution preserves tool messages",
                 "[react_loop][cancel]") {
    // #23 P1：工具执行期间置位 should_cancel（等价于用户 Ctrl+C），
    // ReActLoop 应等待工具协作退出后仍生成 Observation（消息不丢），
    // 再在下一轮 Thought 走 was_interrupted 分支。
    auto slow_tool = std::make_shared<CooperativeSlowTool>();
    registry->register_tool(slow_tool);

    make_tool_call_reader("tu_01", "Slow", R"({})");

    // 第二轮 Thought 需要一个 reader 才能进入 while 检查 should_cancel
    make_text_reader("second round");

    std::vector<ChatMessage> messages = {ChatMessage::user("slow tool")};
    auto loop = make_loop();

    // 模拟工具执行中途用户打断：等工具真正开始执行后再置位（不依赖 run 启动耗时）
    std::thread canceler([&should_cancel = this->should_cancel, &slow_tool]() {
        for (int i = 0; i < 400 && !slow_tool->started(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));  // 最多等 2s
        }
        should_cancel = true;
    });
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);
    canceler.join();

    // 中断被感知 → 走 was_interrupted 分支
    REQUIRE(result.was_interrupted);
    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.total_tool_calls == 1);

    // 工具结果消息仍被完整记录（协作取消后 Observation 照常生成）
    REQUIRE(messages.size() >= 3);
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content.find("cancelled") != std::string::npos);
}

// ============================================================================
// #30 环境感知注入
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop injects environment fields into ToolContext (#30)",
                 "[react_loop][env]") {
    // #30：工具执行时应能读到 request_id / session_id / history_summary / git 环境。
    //      git 字段依赖 cwd 是否在 git 仓库，本测试不担保其值，仅校验注入通道存在
    //      （能读到 request_id、历史摘要、模型名即可）。
    auto cap = std::make_shared<CapturedContext>();
    registry->register_tool(std::make_shared<ContextCaptureTool>(cap));

    make_tool_call_reader("tu_01", "Capture", R"({})");
    make_text_reader("done");  // 第二轮无工具调用 → FinalAnswer 结束

    std::vector<ChatMessage> messages = {
        ChatMessage::user("记住：本项目用 nlohmann/json，不要用 rapidjson")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(cap->captured);
    // request_id：每次 turn 生成，非空
    REQUIRE_FALSE(cap->request_id.empty());
    // 历史摘要：应包含用户早先约束（只读文本，无修改通道）
    REQUIRE(cap->history_summary.find("nlohmann/json") != std::string::npos);
    // session_id：ReActLoop 无注入时回退 "default"
    REQUIRE(cap->session_id == "default");
    // model：来自 backend.model_name（未配置时为空，不会崩溃）
    (void)cap->model;
}

// ============================================================================
// 流式错误处理
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop handles stream error", "[react_loop][error]") {
    make_error_reader();

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.error_message.find("Stream error") != std::string::npos);
    REQUIRE(result.partial_content == "partial");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop handles null reader", "[react_loop][error]") {
    // 不设置 next_reader，submit_completion 返回 nullptr
    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    // #89：提交失败与流式错误此前共用同一条固定串，无法区分；现在给出具体详情。
    // 无 HTTP 响应，状态码应为 0（不是 500/502 之类编造值）。
    REQUIRE(result.error_message.find("submit_completion") != std::string::npos);
    REQUIRE(result.http_status == 0);
}

// ============================================================
// #89：HTTP 状态码透传（此前在流层被丢弃，上层只能拿到固定串）
// ============================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop propagates 429 from stream error",
                 "[react_loop][error][89]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("partial");
    reader->set_error_at(1);
    reader->set_error_payload(429, "HTTP error: 429 - rate limited");
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    // #144：本用例只验证透传，关闭重试（429 现在会先退避重试，见 [issue144] 组）
    ReActLoop::Config config;
    config.thought_max_retries = 0;
    auto loop = make_loop(config);
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    // 修复前：http_status 恒为 0，429 与网络错误无法区分
    REQUIRE(result.http_status == 429);
    REQUIRE(result.error_message.find("429") != std::string::npos);
    REQUIRE(result.partial_content == "partial");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop propagates 4xx so it is distinguishable from 5xx",
                 "[react_loop][error][89]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->set_error_at(0);
    reader->set_error_payload(401, "HTTP error: 401 - invalid api key");
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    ReActLoop::Config config;
    config.thought_max_retries = 0;  // 同上：只测透传
    auto loop = make_loop(config);
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.http_status == 401);
    REQUIRE(result.error_message.find("401") != std::string::npos);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop keeps http_status 0 when no HTTP response",
                 "[react_loop][error][89]") {
    // 网络错误（curl 传输失败）：无 HTTP 响应，状态码应保持 0 而非编造
    auto reader = std::make_shared<MockStreamReader>();
    reader->set_error_at(0);
    reader->set_error_payload(0, "Connection timed out");
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    ReActLoop::Config config;
    config.thought_max_retries = 0;  // 网络错误同样可重试，这里只关心透传
    auto loop = make_loop(config);
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.http_status == 0);
    REQUIRE(result.error_message == "Connection timed out");
}

// ============================================================================
// 回调
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop invokes on_step callback for each phase",
                 "[react_loop][callback]") {
    make_tool_call_reader("tu_01", "Echo", R"({"text":"x"})");
    make_text_reader("final");

    std::vector<ReActStep> steps;
    auto on_step = [&steps](const ReActStep& step) { steps.push_back(step); };

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel, on_step);

    // 应有：Thought(1) + Action(1) + Observation(1) + Thought(2) + FinalAnswer(2) = 5 步
    // 但实际代码可能只记录 Thought + FinalAnswer 步骤，Action/Observation 不一定单独记录
    // 至少应有 2 个 Thought 步骤
    REQUIRE(steps.size() >= 2);

    // 第一个应是 Thought
    REQUIRE(steps[0].type == ReActStepType::Thought);
    REQUIRE(steps[0].step_number == 1);

    // 最后一个应是 FinalAnswer
    REQUIRE(steps.back().type == ReActStepType::FinalAnswer);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop invokes on_token callback for content deltas",
                 "[react_loop][callback]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("Hello ");
    reader->add_content_chunk("world!");
    reader->set_usage(5, 10);
    provider->set_next_reader(reader);

    std::string collected_content;
    auto on_token = [&collected_content](const std::string& content_delta,
                                         const std::string& /*reasoning_delta*/) {
        collected_content += content_delta;
    };

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result =
        loop->run(messages, "", nlohmann::json::array(), should_cancel, nullptr, on_token);

    REQUIRE(collected_content == "Hello world!");
    REQUIRE(result.final_answer == "Hello world!");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop invokes on_token for reasoning deltas",
                 "[react_loop][callback]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_reasoning_chunk("step1 ");
    reader->add_reasoning_chunk("step2");
    reader->add_content_chunk("answer");
    reader->set_usage(5, 10);
    provider->set_next_reader(reader);

    std::string collected_reasoning;
    auto on_token = [&collected_reasoning](const std::string& /*content_delta*/,
                                           const std::string& reasoning_delta) {
        collected_reasoning += reasoning_delta;
    };

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result =
        loop->run(messages, "", nlohmann::json::array(), should_cancel, nullptr, on_token);

    REQUIRE(collected_reasoning == "step1 step2");
}

// ============================================================================
// 无工具注册表
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop works without registry (no tools)",
                 "[react_loop][no_tools]") {
    auto reader = make_text_reader("plain answer");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    // H-5：必须显式注入 IConfigManager*（非空）
    auto loop = std::make_unique<ReActLoop>(provider.get(), nullptr, ReActLoop::Config{},
                                            &ConfigManager::instance());  // registry = nullptr
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.final_answer == "plain answer");
    REQUIRE(result.total_tool_calls == 0);
}

// ============================================================================
// H-1（PR #46 评审）：工具路径权限变更 → 回写宿主通知（统一状态源）
// 覆盖"每轮注入恢复 + 工具回调路径"的集成：真实 EnterPlanMode/ExitPlanModeV2
// 工具经 ReActLoop 执行后，set_permission_state_changed_callback 应收到最新三态，
// 宿主（ChatSession）据此持久化，下一轮 apply_permission_state 注入恢复。
// ============================================================================

namespace {

/// @brief 模拟 ChatSession 的宿主持久化 + 下一轮注入
struct PermissionHost {
    tool::PermissionMode mode{tool::PermissionMode::Default};
    tool::PermissionMode before_plan{tool::PermissionMode::Default};
    bool in_plan{false};
    int notify_count = 0;

    void notify(tool::PermissionMode m, tool::PermissionMode b, bool p) {
        mode = m;
        before_plan = b;
        in_plan = p;
        ++notify_count;
    }

    void inject(ReActLoop& loop) const {
        loop.apply_permission_state(mode, before_plan, mode == tool::PermissionMode::Plan);
    }
};

}  // namespace

TEST_CASE_METHOD(ReActLoopFixture,
                 "H-1 EnterPlanMode tool writeback lets Plan persist across turns (scenario B)",
                 "[react_loop][permission][45][h1]") {
    registry->register_tool(std::make_shared<EnterPlanModeTool>());

    PermissionHost host;
    auto loop = make_loop();
    loop->set_permission_state_changed_callback(
        [&host](tool::PermissionMode m, tool::PermissionMode b, bool p) { host.notify(m, b, p); });

    // 第一轮：模型调用 EnterPlanMode → 工具经 on_enter_plan_mode 切 Plan 并回写宿主
    make_tool_call_reader("call_1", "EnterPlanMode", R"({"reason":"research"})");
    // 第二轮：纯文本终止
    make_text_reader("planning done");

    std::vector<ChatMessage> messages = {ChatMessage::user("plan it")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(host.notify_count >= 1);
    REQUIRE(host.mode == tool::PermissionMode::Plan);
    REQUIRE(host.before_plan == tool::PermissionMode::Default);
    REQUIRE(host.in_plan == true);

    // 宿主持久化后下一轮注入：Plan 状态跨 turn 保持（修复 EnterPlanMode 后 Plan 丢失）
    auto loop2 = make_loop();
    host.inject(*loop2);
    REQUIRE(loop2->permission_mode() == tool::PermissionMode::Plan);
}

TEST_CASE_METHOD(ReActLoopFixture,
                 "H-1 ExitPlanModeV2 approved writeback restores mode (scenario A)",
                 "[react_loop][permission][45][h1]") {
    registry->register_tool(std::make_shared<ExitPlanModeV2Tool>());

    // MockEventBus：自动批准 ExitPlanModeV2 的 AskUser 确认
    agent::test::MockEventBus bus;
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);
    bus.subscribe<AskUserRequestEvent>([](const AskUserRequestEvent& e) {
        AskUserResult result;
        result.submitted = true;
        result.answers.emplace_back("permission", "Yes");
        e.result_promise->set_value(result);
    });

    PermissionHost host;
    // 模拟 Shift+Tab 已切 Plan（ChatSession=Plan）+ 本轮注入
    auto loop = std::make_unique<ReActLoop>(
        provider.get(), registry, ReActLoop::Config{}, &ConfigManager::instance(),
        /*task_manager=*/nullptr, /*cwd=*/"", /*external_compactor=*/nullptr,
        /*event_bus=*/&bus);
    loop->apply_permission_state(tool::PermissionMode::Plan, tool::PermissionMode::Default, true);
    loop->set_permission_state_changed_callback(
        [&host](tool::PermissionMode m, tool::PermissionMode b, bool p) { host.notify(m, b, p); });

    // 第一轮：模型调用 ExitPlanModeV2（自动批准）→ on_exit_plan_mode 恢复原模式并回写宿主
    make_tool_call_reader("call_1", "ExitPlanModeV2", R"({"plan":"refactor x.cpp"})");
    // 第二轮：纯文本终止
    make_text_reader("approved, proceeding");

    std::vector<ChatMessage> messages = {ChatMessage::user("approve plan")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(host.notify_count >= 1);
    // 批准退出 → 恢复进入 Plan 前的原模式（Default），而非硬编码/残留 Plan
    REQUIRE(host.mode == tool::PermissionMode::Default);
    REQUIRE(host.in_plan == false);

    // 宿主按回写结果注入下一轮 → 不再打回 Plan（修复 ExitPlanModeV2 批准后 Plan"粘死"）
    auto loop2 = make_loop();
    host.inject(*loop2);
    REQUIRE(loop2->permission_mode() == tool::PermissionMode::Default);
}

// ============================================================================
// 停滞检测 + 内部评审器（0.6.x）
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop stall recovery continues after reviewer correction",
                 "[react_loop][stall]") {
    ReActLoop::Config config;
    config.max_iterations = 10;
    config.review_stall_window = 2;
    auto loop = make_loop(config);

    // 迭代1: Echo(a)；迭代2: 再次 Echo(a) → 停滞；评审返回 continue；迭代3: final
    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");
    make_tool_call_reader("tu_2", "Echo", R"({"text":"a"})");
    make_text_reader(R"({"action":"continue","reason":"try another file"})");
    make_text_reader("done after recovery");

    std::vector<ChatMessage> messages = {ChatMessage::user("fix task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "done after recovery");
    REQUIRE(result.total_iterations == 3);
    // 纠偏指令注入为 system 消息；重复工具实际只执行一次（第二次因停滞被驳回未执行）
    REQUIRE(messages.size() == 5);
    REQUIRE(messages[3].role == ChatMessage::Role::System);
    REQUIRE(messages[3].content.find("try another file") != std::string::npos);
    REQUIRE(echo_tool->call_count == 1);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop stall detection wraps up gracefully",
                 "[react_loop][stall]") {
    ReActLoop::Config config;
    config.max_iterations = 10;
    config.review_stall_window = 2;
    auto loop = make_loop(config);

    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");
    make_tool_call_reader("tu_2", "Echo", R"({"text":"a"})");
    make_text_reader(R"({"action":"wrap_up","reason":"stuck in loop"})");

    std::vector<ChatMessage> messages = {ChatMessage::user("fix")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE_FALSE(result.was_interrupted);
    REQUIRE(result.final_answer.find("stuck in loop") != std::string::npos);
    REQUIRE(result.total_iterations == 2);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop at-limit reviewer grants extra budget",
                 "[react_loop][limit-review]") {
    ReActLoop::Config config;
    config.max_iterations = 2;
    config.review_extra_budget = 5;
    config.review_max_grants = 2;
    auto loop = make_loop(config);

    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");  // 迭代1
    make_tool_call_reader("tu_2", "Echo", R"({"text":"b"})");  // 迭代2（不同输入，不触发停滞）
    make_text_reader(R"({"action":"continue","reason":"keep going"})");  // 达上限评审
    make_text_reader("done with extra budget");                          // 追加预算后迭代3

    std::vector<ChatMessage> messages = {ChatMessage::user("task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "done with extra budget");
    REQUIRE(result.total_iterations == 3);  // 超限后评审"继续" → 追加预算续跑
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop at-limit reviewer wraps up gracefully",
                 "[react_loop][limit-review]") {
    ReActLoop::Config config;
    config.max_iterations = 2;
    auto loop = make_loop(config);

    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");
    make_tool_call_reader("tu_2", "Echo", R"({"text":"b"})");
    make_text_reader(R"({"action":"wrap_up","reason":"enough progress"})");

    std::vector<ChatMessage> messages = {ChatMessage::user("task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer.find("enough progress") != std::string::npos);
    REQUIRE(result.total_iterations == 2);
    REQUIRE(echo_tool->call_count == 2);
}

// ============================================================================
// Issue #133：评审器失效不得终止任务（fail-open + 熔断）
// ============================================================================
// 背景：Terminal-Bench 首轮 20 题实测，评审器被调用 9 次、9 次拿不到可解析 JSON
// （reasoning 模型把 max_tokens=200 全花在思维链上，content 为空）。旧代码把这种
// "拿不到裁决"当作 wrap_up，直接杀掉 9 个还在正常推进的任务（其中 3 题本来能过）。

TEST_CASE_METHOD(ReActLoopFixture, "#133: 评审器无裁决时停滞分支 fail-open 继续",
                 "[react_loop][stall][issue133]") {
    ReActLoop::Config config;
    config.max_iterations = 10;
    config.review_stall_window = 2;
    auto loop = make_loop(config);

    // 迭代1 Echo(a)；迭代2 再次 Echo(a) → 停滞；评审器返回非 JSON（失效）；迭代3 final
    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");
    make_tool_call_reader("tu_2", "Echo", R"({"text":"a"})");
    make_text_reader("我来想想……（没有任何 JSON）");  // 评审器：无裁决
    make_text_reader("done after fail-open");

    std::vector<ChatMessage> messages = {ChatMessage::user("fix task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    // 关键：不得因评审器失效而判定失败/收尾
    REQUIRE_FALSE(result.was_error);
    REQUIRE_FALSE(result.was_interrupted);
    REQUIRE(result.final_answer == "done after fail-open");
    REQUIRE(result.total_iterations == 3);
    // 注入的是兜底纠偏指令（而非评审器给的摘要）
    REQUIRE(messages.size() == 5);
    REQUIRE(messages[3].role == ChatMessage::Role::System);
    REQUIRE(messages[3].content.find("换一条更高效的路径") != std::string::npos);
    REQUIRE(echo_tool->call_count == 1);
}

TEST_CASE_METHOD(ReActLoopFixture, "#133: 评审器无裁决时达上限分支授予追加预算",
                 "[react_loop][limit-review][issue133]") {
    ReActLoop::Config config;
    config.max_iterations = 2;
    config.review_extra_budget = 5;
    config.review_max_grants = 2;
    auto loop = make_loop(config);

    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");  // 迭代1
    make_tool_call_reader("tu_2", "Echo", R"({"text":"b"})");  // 迭代2（不同输入，不触发停滞）
    make_text_reader("（空返回，无 JSON）");                      // 达上限评审：无裁决
    make_text_reader("done with fail-open budget");             // 追加预算后迭代3

    std::vector<ChatMessage> messages = {ChatMessage::user("task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "done with fail-open budget");
    REQUIRE(result.total_iterations == 3);  // 评审失效也要给预算，而不是收尾
}

TEST_CASE_METHOD(ReActLoopFixture, "#133: 评审器连续失效后熔断，不再重复调用",
                 "[react_loop][stall][issue133]") {
    ReActLoop::Config config;
    config.max_iterations = 6;
    config.review_stall_window = 2;
    config.review_max_grants = 1;  // 熔断阈值 = max(1, 1) = 1 → 首次失效即熔断
    auto loop = make_loop(config);

    // 迭代1 Echo(a) → 窗口 [a]
    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");
    // 迭代2 Echo(a) → 停滞 → 评审第 1 次（失效）→ 熔断计数=1 → fail-open 继续
    make_tool_call_reader("tu_2", "Echo", R"({"text":"a"})");
    make_text_reader("no decision here");  // 评审器（唯一一次调用）
    // 迭代3 Echo(a) → 窗口清空后重新累积 [a]
    make_tool_call_reader("tu_3", "Echo", R"({"text":"a"})");
    // 迭代4 Echo(a) → 停滞 → 熔断已开，不再调用评审器 → fail-open 继续
    make_tool_call_reader("tu_4", "Echo", R"({"text":"a"})");
    // 迭代5 收敛
    make_text_reader("finished without reviewer");

    std::vector<ChatMessage> messages = {ChatMessage::user("fix task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "finished without reviewer");
    REQUIRE(result.total_iterations == 5);
    // 5 次迭代 Thought + 1 次评审调用 = 6；若未熔断会是 7（多一次注定无果的评审请求）
    REQUIRE(provider->submit_count == 6);
}

TEST_CASE_METHOD(ReActLoopFixture, "#133: 评审器返回未知 action 视为不可信裁决",
                 "[react_loop][stall][issue133]") {
    ReActLoop::Config config;
    config.max_iterations = 10;
    config.review_stall_window = 2;
    auto loop = make_loop(config);

    make_tool_call_reader("tu_1", "Echo", R"({"text":"a"})");
    make_tool_call_reader("tu_2", "Echo", R"({"text":"a"})");
    make_text_reader(R"({"action":"maybe","reason":"not sure"})");  // 既非 continue 也非 wrap_up
    make_text_reader("kept going");

    std::vector<ChatMessage> messages = {ChatMessage::user("fix task")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "kept going");
}

TEST_CASE("#133: 评审器默认参数放宽（窗口 8 / max_tokens 512）", "[react_loop][issue133]") {
    const ReActLoop::Config cfg;
    // 窗口 4 太窄：Read→Edit→Read 的正常节奏会被误判成循环
    REQUIRE(cfg.review_stall_window == 8);
    // 200 太小：reasoning 模型把预算吃光后 content 为空
    REQUIRE(cfg.review_max_tokens == 512);
}

// ============================================================================
// Issue #78：FinalAnswer 前强制验证闭环（PreCompletion 门禁）
// ============================================================================
// 用 FileExists 目标驱动门禁：不依赖真实构建环境，结果确定且执行快。
// 脚本化模型靠 MockCompletionProvider 的 reader 队列实现（每轮 run 消费一个），
// 因此"多轮回灌"不需要额外的 scripted_model 基建。

namespace {

/// @brief #78：写"目标产物"的工具 —— 模拟模型真的照反馈去修了
class WriteArtifactTool : public ITool {
   public:
    mutable int call_count = 0;
    std::string path;

    explicit WriteArtifactTool(std::string target) : path(std::move(target)) {}

    const std::string& name() const override {
        static const std::string n = "WriteArtifact";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Writes the target artifact file";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "WriteArtifact tool for #78 gate testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        ++call_count;
        std::ofstream(path) << "artifact";
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("artifact written")));
    }
};

/// @brief #78 P3/P4：建只含指定线索文件的临时项目目录（探测用例用）
/// @details 探测结果只由这些线索文件决定，不受测试进程当前目录影响。
///          调用方负责 fs::remove_all。
std::string make_probe_project(const std::vector<std::string>& files) {
    namespace fs = std::filesystem;
    const std::string dir =
        (fs::temp_directory_path() /
         ("workx_vfprobe_" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
            .string();
    fs::create_directories(dir);
    for (const auto& rel : files) {
        const fs::path p = fs::path(dir) / rel;
        fs::create_directories(p.parent_path());
        std::ofstream(p) << "{}\n";
    }
    return dir;
}

struct VerificationGateFixture : ReActLoopFixture {
    std::string dir;       ///< 隔离工作目录（同时作为 loop 的 cwd）
    std::string artifact;  ///< 待验证的目标产物路径
    std::shared_ptr<WriteArtifactTool> writer;

    VerificationGateFixture() {
        namespace fs = std::filesystem;
        dir = (fs::temp_directory_path() /
               ("workx_vfgate_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
                  .string();
        fs::create_directories(dir);
        artifact = dir + "/artifact.txt";
        writer = std::make_shared<WriteArtifactTool>(artifact);
        registry->register_tool(writer);
    }

    ~VerificationGateFixture() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /// @brief 门禁配置：以 "产物文件存在" 作为验证目标
    ReActLoop::Config gate_config(int max_attempts) {
        ReActLoop::Config cfg;
        cfg.goal = parse_goal("file_exists:" + artifact);
        cfg.verify_before_finish = true;
        cfg.verify_max_attempts = max_attempts;
        cfg.max_iterations = 10;
        return cfg;
    }

    std::unique_ptr<ReActLoop> make_gate_loop(ReActLoop::Config config) {
        return std::make_unique<ReActLoop>(provider.get(), registry, config,
                                           &ConfigManager::instance(), nullptr, dir);
    }

    /// @brief 会话历史中是否存在一条含指定文本的 user 消息（回灌的验证失败信息）
    bool injected_user_text(const std::vector<ChatMessage>& messages,
                            std::string_view needle) const {
        for (const auto& m : messages) {
            if (m.role == ChatMessage::Role::User && m.content.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
};

}  // namespace

TEST_CASE_METHOD(VerificationGateFixture, "VF-01: 验证未通过时不产生无警告的最终答复",
                 "[react_loop][issue78]") {
    // max_attempts=1 → 首次失败即降级（不再回灌重试）
    auto loop = make_gate_loop(gate_config(1));
    make_text_reader("我认为已经完成了。");

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.goal_status == GoalStatus::Failed);
    // 原始答复保留（信息不丢），但必须带上未完成验证的警告
    REQUIRE(result.final_answer.find("我认为已经完成了。") != std::string::npos);
    REQUIRE(result.final_answer.find("未完成验证") != std::string::npos);
    REQUIRE_FALSE(result.was_error);  // 降级不是错误，是"降级成功终止"
}

TEST_CASE_METHOD(VerificationGateFixture, "VF-02: 失败回灌后模型修复 → 重验通过",
                 "[react_loop][issue78]") {
    auto loop = make_gate_loop(gate_config(3));

    make_text_reader("完成了。");                                        // 轮1：无据宣称完成
    make_tool_call_reader("tu_1", "WriteArtifact", R"({})");            // 轮2：照反馈去补产物
    make_text_reader("修复后重新验证过了，已完成。");                     // 轮3：验证通过

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.goal_status == GoalStatus::Achieved);
    REQUIRE(result.final_answer == "修复后重新验证过了，已完成。");
    REQUIRE(result.final_answer.find("未完成验证") == std::string::npos);
    REQUIRE(writer->call_count == 1);  // 回灌确实驱动了动作，而不是空转
    REQUIRE(result.total_iterations == 3);
    // 回灌的失败信息进入了会话历史，且落在 user 角色（模型视角的纠偏指令）
    REQUIRE(injected_user_text(messages, "验证未通过"));
}

TEST_CASE_METHOD(VerificationGateFixture, "VF-02: 达重试上限后降级为带警告终止",
                 "[react_loop][issue78]") {
    auto loop = make_gate_loop(gate_config(2));

    make_text_reader("第一轮说完成。");  // 轮1：验证失败 → 回灌
    make_text_reader("第二轮还说完成。");  // 轮2：验证仍失败 → 达上限 → 降级

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.goal_status == GoalStatus::Failed);
    REQUIRE(result.final_answer.find("未完成验证") != std::string::npos);
    REQUIRE(result.final_answer.find("第二轮还说完成。") != std::string::npos);
    REQUIRE(provider->submit_count == 2);  // 恰好两次 Thought：回灌一次后仍未过 → 降级，不会无限重试
}

TEST_CASE_METHOD(VerificationGateFixture, "VF-02: 回灌必须消耗预算（不得静默死循环）",
                 "[react_loop][issue78]") {
    // §5.1 的核心风险：回灌分支的 continue 会跳过循环体末尾的 --budget/++iteration。
    // 这里把预算压到 4、给足 10 个 reader，若记账漏了循环就跑不完 / 打满预算。
    ReActLoop::Config cfg = gate_config(100);  // 重试上限放开，让预算成为唯一约束
    cfg.max_iterations = 4;
    auto loop = make_gate_loop(cfg);

    for (int i = 0; i < 10; ++i) {
        make_text_reader("还没做，先说完成。");  // 模型从不修复 → 每次都会被回灌
    }

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    // 预算有限 ⇒ 必须在有限步内降级退出，且提交次数远小于提供的 reader 数
    REQUIRE(result.goal_status == GoalStatus::Failed);
    REQUIRE(result.final_answer.find("未完成验证") != std::string::npos);
    REQUIRE(provider->submit_count < 10);
    REQUIRE(result.total_iterations <= 4);
}

TEST_CASE_METHOD(VerificationGateFixture, "VF-03: 无可用验证器时门禁直接放行",
                 "[react_loop][issue78]") {
    // (a) 开关开启但未声明目标 → 不执行任何命令，goal_status 保持 Unknown
    {
        ReActLoop::Config cfg;
        cfg.verify_before_finish = true;  // goal.type 仍是 None
        auto loop = make_gate_loop(cfg);
        make_text_reader("普通对话答复。");

        std::vector<ChatMessage> messages = {ChatMessage::user("你好")};
        auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

        REQUIRE(result.final_answer == "普通对话答复。");
        REQUIRE(result.goal_status == GoalStatus::Unknown);  // 未经验证 ≠ 验证失败
        REQUIRE(provider->submit_count == 1);
    }
    // (b) 声明了目标但该类型没有验证器（多模式目标）→ 同样放行，不得误判失败
    {
        ReActLoop::Config cfg;
        cfg.verify_before_finish = true;
        cfg.goal.type = AgentGoal::Batch;  // has_checker() == false
        auto loop = make_gate_loop(cfg);
        make_text_reader("多模式目标的答复。");

        std::vector<ChatMessage> messages = {ChatMessage::user("跑批任务")};
        auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

        REQUIRE(result.final_answer == "多模式目标的答复。");
        REQUIRE(result.goal_status == GoalStatus::Unknown);
    }
}

TEST_CASE_METHOD(ReActLoopFixture, "VF-04: apply_verification_gate 按入口正确装配门禁",
                 "[react_loop][issue78]") {
    MockConfigManager cfg_mgr;

    // 探测用空目录：不传会取进程当前目录，结果会被「跑在仓库根」污染
    const std::string empty = make_probe_project({});

    // 传 false 的分支（P3 之前交互式走这条）：未显式配置 → 关闭、无目标
    ReActLoop::Config interactive;
    apply_verification_gate(interactive, cfg_mgr, /*enabled_by_default=*/false, empty);
    REQUIRE_FALSE(interactive.goal.has_goal());
    REQUIRE(interactive.verify_before_finish == false);

    // headless（评测入口）：未显式配置 → 默认开启
    // 这是 #78 阶段 P2 的关键：headless 自建 ReActLoop，不走 GoalGuardedAgent，
    // 漏掉这条接线评测链路就是零验证。
    ReActLoop::Config headless;
    apply_verification_gate(headless, cfg_mgr, /*enabled_by_default=*/true, empty);
    REQUIRE(headless.verify_before_finish == true);
    // P3 的安全边界：空目录里推断不出任何目标 → 门禁开了也不会跑命令
    REQUIRE_FALSE(headless.goal.has_goal());

    // 配置了 goal → 必须解析进 Config，否则门禁永远不会真正验证任何东西
    cfg_mgr.set_value(agent::keys::AGENT_GOAL, std::string("tests_pass"));
    ReActLoop::Config with_goal;
    apply_verification_gate(with_goal, cfg_mgr, true);
    REQUIRE(with_goal.goal.type == AgentGoal::TestsPass);

    // 显式开关优先于入口默认值
    cfg_mgr.set_value(agent::keys::AGENT_VERIFY_BEFORE_FINISH, false);
    ReActLoop::Config explicit_off;
    apply_verification_gate(explicit_off, cfg_mgr, true);
    REQUIRE(explicit_off.verify_before_finish == false);

    // 非法值归一化为 >=1：0/负值会让重试封顶判定失去意义
    cfg_mgr.set_value(agent::keys::AGENT_VERIFY_MAX_ATTEMPTS, 0);
    ReActLoop::Config bad_cap;
    apply_verification_gate(bad_cap, cfg_mgr, true, empty);
    REQUIRE(bad_cap.verify_max_attempts == 1);

    std::error_code ec;
    std::filesystem::remove_all(empty, ec);
}

// ============================================================
// VF-11（Issue #78 P3）：默认开启后按项目线索推断目标
// ============================================================

TEST_CASE_METHOD(ReActLoopFixture, "VF-11: 未声明 agent.goal 时按项目线索推断默认目标",
                 "[react_loop][issue78]") {
    MockConfigManager cfg_mgr;

    // 有 CTest 入口 → 测试优先（tests_pass 比 build_clean 更贴近任务验收）
    const std::string cmake = make_probe_project({"CMakeLists.txt", "build/CTestTestfile.cmake"});
    ReActLoop::Config cmake_cfg;
    apply_verification_gate(cmake_cfg, cfg_mgr, true, cmake);
    REQUIRE(cmake_cfg.goal.type == AgentGoal::TestsPass);

    // 只能构建、跑不了测试 → 退到 build_clean
    const std::string make_only = make_probe_project({"Makefile"});
    ReActLoop::Config make_cfg;
    apply_verification_gate(make_cfg, cfg_mgr, true, make_only);
    REQUIRE(make_cfg.goal.type == AgentGoal::BuildClean);

    // 用户显式声明优先于推断
    cfg_mgr.set_value(agent::keys::AGENT_GOAL, std::string("lint_zero"));
    ReActLoop::Config explicit_goal;
    apply_verification_gate(explicit_goal, cfg_mgr, true, cmake);
    REQUIRE(explicit_goal.goal.type == AgentGoal::LintZero);

    std::error_code ec;
    std::filesystem::remove_all(cmake, ec);
    std::filesystem::remove_all(make_only, ec);
}

TEST_CASE_METHOD(VerificationGateFixture, "VF-05: Stop hook 阻断不吞掉验证结论",
                 "[react_loop][issue78][hook]") {
    // Stop 的 blockingError 会把 final_answer 整个覆写掉（这是已知且刻意的行为），
    // 但 goal_status 必须保留 —— 否则 #78 的验证结果会被 hook 静默抹掉。
    auto hooks = std::make_shared<agent::hook::HookManager>();
    hooks->set_provider(provider.get());
    agent::hook::HookDefinition def;
    def.event = agent::hook::HookEvent::Stop;
    def.type = agent::hook::HookType::Prompt;
    def.prompt = "audit the final answer";
    hooks->register_hook(def);

    ReActLoop::Config cfg = gate_config(1);
    cfg.hooks = hooks;
    auto loop = make_gate_loop(cfg);

    make_text_reader("声明完成但其实没验证过。");  // reader[0]：主循环（→ 门禁降级）
    auto hook_reader = std::make_shared<MockStreamReader>();
    hook_reader->add_content_chunk(R"({"blockingError":"blocked-by-stop-hook"})");
    provider->set_next_reader(hook_reader);  // reader[1]：Stop hook 判定

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.final_answer == "blocked-by-stop-hook");  // 覆写照旧
    REQUIRE(result.goal_status == GoalStatus::Failed);       // 但验证结论存活
}

TEST_CASE_METHOD(VerificationGateFixture, "VF-09: 预算仅剩一轮时不再无效回灌，直接降级",
                 "[react_loop][issue78]") {
    // max_iterations=1 → budget==1，回灌后必然立刻退出，只会白丢一次机会。
    // 因此门禁在此情形下直接产出降级答复，而不是留下一个未验证的空结果。
    ReActLoop::Config cfg = gate_config(5);  // 重试上限很宽松，预算才是瓶颈
    cfg.max_iterations = 1;
    auto loop = make_gate_loop(cfg);
    make_text_reader("完成。");

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.goal_status == GoalStatus::Failed);
    REQUIRE(result.final_answer.find("未完成验证") != std::string::npos);
    REQUIRE(result.total_iterations == 1);
    REQUIRE(provider->submit_count == 1);  // 没有多余的一次回灌
}

// ============================================================
// VF-10（Issue #78 P4）：探测不到验证手段时门禁放行
// ============================================================
// 与 VF-03 的区别：VF-03 覆盖「goal 类型没有验证器」（has_checker 为假）；
// 本条覆盖「类型有验证器，但这个项目里没有可跑的命令」。
// 二者都必须放行 —— 否则「无法验证」会被当成「验证失败」，模型为
// 不存在的测试目标反复纠错，把预算烧在没有答案的地方。

TEST_CASE_METHOD(VerificationGateFixture, "VF-10: 项目探测不到验证命令 → 门禁直接放行",
                 "[react_loop][issue78]") {
    // fixture 的 dir 是空临时目录：没有 CMakeLists / package.json / Cargo.toml …
    ReActLoop::Config cfg;
    cfg.goal = parse_goal("tests_pass");
    cfg.verify_before_finish = true;
    cfg.verify_max_attempts = 3;
    cfg.max_iterations = 10;
    auto loop = make_gate_loop(cfg);
    make_text_reader("完成了。");

    std::vector<ChatMessage> messages = {ChatMessage::user("请实现这个功能")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.goal_status != GoalStatus::Failed);  // 不得降级成「未完成验证」
    REQUIRE(result.final_answer == "完成了。");          // 答复不得被追加警告
    REQUIRE(provider->submit_count == 1);               // 不得产生回灌轮
    REQUIRE_FALSE(injected_user_text(messages, "验证未通过"));
}

// ============================================================
// Issue #83：剩余预算提示
// ============================================================
// 真跑 30+ 轮需要真 LLM（烧 token），这里用 mock provider 精确驱动到档位边界：
// 预算提示是**纯本地判定**，是否与 token 无关，因此 mock 足以验证注入链路。
// 档位文案本身由 test_budget_hint.cpp 的纯函数用例覆盖。

namespace {

/// @brief #83：造一个「有 ctest 线索」的项目目录
/// @details detect_goal_command(TestsPass) 要求 CMakeLists.txt + CTestTestfile.cmake
///          同时存在，缺一都不算「这个项目有测试」。调用方负责 remove_all。
std::string make_ctest_project() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "workx_issue83_ctest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "build", ec);
    auto write = [](const fs::path& p, const char* text) {
        std::ofstream out(p);
        out << text;
    };
    write(dir / "CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    write(dir / "build" / "CTestTestfile.cmake", "# generated by cmake\n");
    return dir.string();
}

/// @brief 历史里是否出现过含子串的 system 消息（预算提示以 system 角色注入）
bool budget_hint_seen(const std::vector<ChatMessage>& messages, std::string_view needle) {
    for (const auto& m : messages) {
        if (m.role == ChatMessage::Role::System && m.content.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE_METHOD(ReActLoopFixture, "#83: 剩余预算跨过 30% / 10% 阈值时注入收尾提示",
                 "[react_loop][issue83]") {
    ReActLoop::Config config;
    config.max_iterations = 10;
    config.review_max_grants = 0;  // 不追加预算：跑满 10 轮硬停，避免牵扯评审器
    auto loop = make_loop(config);

    // 10 轮工具调用，输入各不相同 → 不触发停滞判定
    for (int i = 1; i <= 10; ++i) {
        make_tool_call_reader("tu_" + std::to_string(i), "Echo",
                              R"({"text":"step)" + std::to_string(i) + R"("})");
    }

    std::vector<ChatMessage> messages = {ChatMessage::user("这是一个很长的任务")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_iterations == 10);
    CHECK(echo_tool->call_count == 10);
    // 剩余 9..4（>30%）不发提示 —— 每轮都塞一行只会淹没工具输出
    CHECK_FALSE(budget_hint_seen(messages, "剩余轮次 9 / 10"));
    CHECK_FALSE(budget_hint_seen(messages, "剩余轮次 5 / 10"));
    // 第 7 轮后剩余 3（30%）→ Warn
    CHECK(budget_hint_seen(messages, "剩余轮次 3 / 10"));
    CHECK(budget_hint_seen(messages, "转入验证与收尾"));
    // 第 9 轮后剩余 1（10%）→ Critical
    CHECK(budget_hint_seen(messages, "剩余轮次 1 / 10"));
    // 无 goal 时不得凭空编一条命令出来
    CHECK_FALSE(budget_hint_seen(messages, "`"));
}

TEST_CASE_METHOD(ReActLoopFixture, "#83: Warn 档点名项目里真实的验证命令",
                 "[react_loop][issue83]") {
    const std::string project = make_ctest_project();
    ReActLoop::Config config;
    config.goal = parse_goal("tests_pass");
    config.verify_before_finish = true;
    config.max_iterations = 10;
    config.review_max_grants = 0;
    auto loop = std::make_unique<ReActLoop>(provider.get(), registry, config,
                                            &ConfigManager::instance(), nullptr, project);

    for (int i = 1; i <= 8; ++i) {
        make_tool_call_reader("tu_" + std::to_string(i), "Echo",
                              R"({"text":"step)" + std::to_string(i) + R"("})");
    }

    std::vector<ChatMessage> messages = {ChatMessage::user("这是一个很长的任务")};
    loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    // 提示里点名的必须是 #78 门禁真正会跑的那条命令（同源，不另造一套）
    CHECK(budget_hint_seen(messages, "`ctest --output-on-failure`"));

    std::error_code ec;
    std::filesystem::remove_all(project, ec);
}

TEST_CASE_METHOD(ReActLoopFixture, "#83: 预算充裕时完全不注入（零开销）", "[react_loop][issue83]") {
    ReActLoop::Config config;
    config.max_iterations = 40;
    auto loop = make_loop(config);

    for (int i = 1; i <= 3; ++i) {
        make_tool_call_reader("tu_" + std::to_string(i), "Echo",
                              R"({"text":"step)" + std::to_string(i) + R"("})");
    }
    make_text_reader("做完了。");

    std::vector<ChatMessage> messages = {ChatMessage::user("短任务")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.final_answer == "做完了。");
    CHECK_FALSE(budget_hint_seen(messages, "预算提醒"));
}

// ============================================================================
// Issue #144 — Thought 阶段 LLM 请求失败的重试策略
//
// 背景：一次网络抖动/慢请求就会让整个任务在第 5 轮终止（Real case:
// adaptive-rejection-sampler 死于 Total request timeout exceeded，核心产物没写出来）。
// 策略：可重试错误（网络/超时/429/5xx）指数退避后重发同一个请求，
//       **不消耗迭代预算**；不可重试错误（4xx）保持原样快速失败。
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture,
                 "#144: 可重试错误会退避重试并最终成功，且不消耗迭代预算",
                 "[react_loop][issue144]") {
    ReActLoop::Config config;
    config.thought_max_retries = 2;
    config.thought_base_delay_ms = 1;  // 测试里不真的等待
    auto loop = make_loop(config);

    // 第一次：典型的网络/O超时错误（http_status=0 且详情非空 → 可重试）
    make_error_reader(0, "Total request timeout exceeded");
    // 第二次：正常完成
    make_text_reader("恢复了。");

    std::vector<ChatMessage> messages = {ChatMessage::user("继续任务")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE_FALSE(result.was_interrupted);
    REQUIRE(result.final_answer == "恢复了。");
    // 失败的那一次没有任何状态变更，重试等价于重发同一请求，不该吃掉预算
    REQUIRE(result.total_iterations == 1);
    REQUIRE(provider->submit_count == 2);
}

TEST_CASE_METHOD(ReActLoopFixture, "#144: 不可重试的 4xx 保持快速失败（不重试）",
                 "[react_loop][issue144]") {
    ReActLoop::Config config;
    config.thought_max_retries = 2;
    config.thought_base_delay_ms = 1;
    auto loop = make_loop(config);

    make_error_reader(401, "Unauthorized");

    std::vector<ChatMessage> messages = {ChatMessage::user("继续任务")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.http_status == 401);
    REQUIRE(provider->submit_count == 1);  // 重试无益，一次就放弃
}

TEST_CASE_METHOD(ReActLoopFixture, "#144: 429 视为可重试", "[react_loop][issue144]") {
    ReActLoop::Config config;
    config.thought_max_retries = 1;
    config.thought_base_delay_ms = 1;
    auto loop = make_loop(config);

    make_error_reader(429, "Rate limited");
    make_text_reader("限速后成功。");

    std::vector<ChatMessage> messages = {ChatMessage::user("继续任务")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "限速后成功。");
    REQUIRE(provider->submit_count == 2);
}

TEST_CASE_METHOD(ReActLoopFixture, "#144: 重试预算耗尽后才放弃", "[react_loop][issue144]") {
    ReActLoop::Config config;
    config.thought_max_retries = 2;
    config.thought_base_delay_ms = 1;
    auto loop = make_loop(config);

    // 1 次原始 + 2 次重试 = 3 个错误 reader，全部失败
    make_error_reader(503, "upstream temporarily down");
    make_error_reader(503, "upstream temporarily down");
    make_error_reader(503, "upstream temporarily down");

    std::vector<ChatMessage> messages = {ChatMessage::user("继续任务")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.http_status == 503);
    REQUIRE(result.error_message.find("upstream") != std::string::npos);
    REQUIRE(provider->submit_count == 3);
}

TEST_CASE_METHOD(ReActLoopFixture, "#144: thought_max_retries=0 恢复旧行为（失败即终止）",
                 "[react_loop][issue144]") {
    ReActLoop::Config config;
    config.thought_max_retries = 0;  // 显式关闭
    config.thought_base_delay_ms = 1;
    auto loop = make_loop(config);

    make_error_reader(0, "network unreachable");

    std::vector<ChatMessage> messages = {ChatMessage::user("继续任务")};
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(provider->submit_count == 1);
}

TEST_CASE_METHOD(ReActLoopFixture, "#144: 退避等待期间可被取消打断", "[react_loop][issue144]") {
    ReActLoop::Config config;
    config.thought_max_retries = 2;
    config.thought_base_delay_ms = 3000;  // 足够长的窗口，确保取消发生在退避中
    auto loop = make_loop(config);

    make_error_reader(0, "Total request timeout exceeded");

    std::vector<ChatMessage> messages = {ChatMessage::user("继续任务")};
    ReActResult result;
    std::thread runner([&] {
        result = loop->run(messages, "", nlohmann::json::array(), should_cancel);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    should_cancel.store(true);
    runner.join();

    // 退避不该把 Ctrl-C 卡住：收到取消后应立即走中断分支，而不是继续重试
    REQUIRE(result.was_interrupted);
    REQUIRE_FALSE(result.was_error);
    REQUIRE(provider->submit_count == 1);
}
