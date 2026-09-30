/**
 * @file test_json_schema_wiring.cpp
 * @brief #80 执行侧接入的端到端验证 —— JS-16「错误回灌后自纠」
 * @details JS-15（执行侧真的调用了校验）已由 test_json_schema.cpp:160 的
 *          [json_schema][executor] 覆盖。本文件补 JS-16：
 *          坏参数被 schema 拦截 → 错误作为 observation 回灌 → 模型换参数重试
 *          → 工具最终执行成功。
 *
 *          与 JS-15 的区别：JS-15 断言「单次调用被拦截」，JS-16 断言
 *          「拦截产生的错误真的回到对话里、且模型据此自纠成功」—— 即闭环。
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/api/chat_types.h"
#include "agent/core/react_loop.h"
#include "agent/tool/itool.h"
#include "agent/tool/registry.h"
#include "core/utils/result_v2.h"

#include "helpers/mock_config_manager.h"
#include "helpers/mock_provider.h"

namespace {

using namespace agent;
using namespace agent::test;

/// @brief 只声明 schema、真正执行时记录被调用的测试工具
/// @details 与 test_json_schema.cpp 的 SchemaOnlyTool 同构：不覆盖 validate_input，
///          完全依赖 ToolExecutor 的统一 schema 校验兜底。
class PathEchoTool : public tool::ITool {
   public:
    mutable int call_count = 0;
    mutable std::string last_path;

    const std::string& name() const override {
        static const std::string n = "PathEchoTool";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "echo a path";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Provide a path.";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", {"path"}},
        };
    }
    ResultV2<tool::ToolResult> call(const nlohmann::json& input,
                                    const tool::ToolContext&) const override {
        ++call_count;
        last_path = input.value("path", "");
        tool::ToolResult r;
        r.text = "ok:" + last_path;
        return ResultV2<tool::ToolResult>::ok(std::move(r));
    }
};

/// @brief 排队一个「调用某工具」的模型回复
void queue_tool_call(MockCompletionProvider& provider, const std::string& id,
                     const std::string& tool_name, const std::string& input_json) {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("calling tool");
    reader->add_tool_use_start(id, tool_name);
    reader->add_tool_use_delta(id, input_json);
    reader->set_usage(10, 20);
    provider.set_next_reader(reader);
}

/// @brief 排队一个纯文本（final answer）回复
void queue_text(MockCompletionProvider& provider, const std::string& text) {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk(text);
    reader->set_usage(10, 20);
    provider.set_next_reader(reader);
}

}  // namespace

TEST_CASE("JS-16 坏参数被拦截后，错误回灌使模型自纠并最终成功",
          "[json_schema][issue80]") {
    MockConfigManager cfg;
    MockCompletionProvider provider;
    auto registry = std::make_shared<tool::ToolRegistry>();
    auto tool = std::make_shared<PathEchoTool>();
    registry->register_tool(tool);

    // 第 1 轮：缺必填 path —— schema 校验应拦截，工具实现不得被执行
    queue_tool_call(provider, "call_1", "PathEchoTool", R"({"not_path":1})");
    // 第 2 轮：模型"看到"错误后改用正确参数
    queue_tool_call(provider, "call_2", "PathEchoTool", R"({"path":"/tmp/x"})");
    // 第 3 轮：收尾
    queue_text(provider, "done");

    // 捕获 Observation 步骤，用于验证错误确实回灌进了对话
    std::vector<ReActStep> observations;
    ReActLoop::StepCallback on_step = [&observations](const ReActStep& s) {
        if (s.type == ReActStepType::Observation) observations.push_back(s);
    };

    ReActLoop loop(&provider, registry, ReActLoop::Config{}, &cfg);

    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user("touch /tmp/x"));
    std::atomic<bool> should_cancel{false};

    const auto result =
        loop.run(messages, "you are a test agent", registry->get_all_schemas(), should_cancel,
                 on_step, /*on_token=*/nullptr);

    REQUIRE(result.was_error == false);

    // 核心：坏参数在第一次调用就被 schema 校验拦住，从未落到工具实现。
    // 若拦截失效，第 1 轮会带着 {"not_path":1} 执行一次，call_count 将是 2。
    REQUIRE(tool->call_count == 1);
    REQUIRE(tool->last_path == "/tmp/x");

    // 两次工具调用 → 两条 observation：第 1 条为错误，第 2 条成功
    REQUIRE(observations.size() == 2);
    REQUIRE(observations[0].is_error == true);
    REQUIRE_FALSE(observations[0].observation.empty());
    REQUIRE(observations[1].is_error == false);
    REQUIRE(observations[1].observation.find("/tmp/x") != std::string::npos);

    // 回灌证据：第一轮的错误文本出现在后续请求的 Tool 消息里
    bool error_fed_back = false;
    for (const auto& m : provider.last_messages) {
        if (m.role == ChatMessage::Role::Tool && m.is_error &&
            m.content == observations[0].observation) {
            error_fed_back = true;
            break;
        }
    }
    REQUIRE(error_fed_back);

    // 三轮各有一次 submit（第 1 轮坏参、第 2 轮好参、第 3 轮收尾）
    REQUIRE(provider.submit_count == 3);
}
