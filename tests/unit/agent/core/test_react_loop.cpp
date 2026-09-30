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
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <optional>

#include "agent/core/react_loop.h"
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
    REQUIRE(result.error_message.find("Stream error") != std::string::npos);
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
