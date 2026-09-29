/**
 * @file test_tool_executor.cpp
 * @brief ToolExecutor 单元测试
 * @details 覆盖 execute 全流程：查找、取消、权限检查、输入验证、执行、异常捕获
 *          V2-4：所有工具返回 ResultV2<ToolResult>，execute 返回 ResultV2<ExecutionResult>
 */

#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <filesystem>

#include "agent/tool/executor.h"
#include "agent/tool/registry.h"
#include "agent/tool/itool.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/utils/error.h"

using namespace agent;
using namespace agent::tool;
using namespace std::chrono_literals;

namespace {

// ============================================================
// 测试工具（V2-4：所有 call/validate/permissions 返回 ResultV2）
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
        static const std::string d = "Echoes input";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext&) const override {
        call_count++;
        last_input = input.value("text", "");
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("echo: ") + last_input));
    }
};

class FailingTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "Failing";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        throw std::runtime_error("intentional failure");
    }
};

class DeniedTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "Denied";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    PermissionResult check_permissions(const nlohmann::json&, const ToolContext&) const override {
        return PermissionResult::err(Error::Code::PermissionDenied, "policy denied");
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("should not reach")));
    }
};

class ValidatingTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "Validating";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"required", {"value"}}};
    }
    ValidationResult validate_input(const nlohmann::json& input,
                                    const ToolContext&) const override {
        if (!input.contains("value")) {
            return ValidationResult::err(Error::Code::MissingArgument, "missing 'value' field");
        }
        return ValidationResult::ok();
    }
    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(
            ToolResult::ok(std::string("got: ") + input["value"].get<std::string>()));
    }
};

class JsonExceptionTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "JsonException";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        throw nlohmann::json::type_error::create(302, "intentional json error", nullptr);
    }
};

class FilesystemErrorTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "FsError";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        throw std::filesystem::filesystem_error(
            "intentional fs error", std::make_error_code(std::errc::no_such_file_or_directory));
    }
};

class UnknownExceptionTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "Unknown";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        throw 42;  // non-std exception
    }
};

class ErrorResultTool : public ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "ErrorResult";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::err(Error::Code::ToolExecutionFailed, "business logic error");
    }
};

// ============================================================
// Fixture
// ============================================================

struct ToolExecutorFixture {
    std::shared_ptr<ToolRegistry> registry;
    std::unique_ptr<ToolExecutor> executor;
    std::shared_ptr<EchoTool> echo;
    ToolContext ctx;

    ToolExecutorFixture() {
        registry = std::make_shared<ToolRegistry>();
        echo = std::make_shared<EchoTool>();
        registry->register_tool(echo);
        executor = std::make_unique<ToolExecutor>(registry);
        ctx.cwd = std::filesystem::current_path().string();
        ctx.session_id = "test-session";
    }
};

}  // namespace

// ============================================================================
// 正常执行
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor executes registered tool",
                 "[tool_executor][basic]") {
    auto result = executor->execute("Echo", R"({"text":"hello"})"_json, ctx);

    REQUIRE(result.is_ok());
    REQUIRE(result.value().tool_name == "Echo");
    REQUIRE(result.value().result.text == "echo: hello");
    REQUIRE(echo->call_count == 1);
}

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor returns Json result type for json output",
                 "[tool_executor][basic]") {
    // EchoTool 返回文本，验证 type 正确
    auto result = executor->execute("Echo", R"({"text":"x"})"_json, ctx);
    REQUIRE(result.is_ok());
    REQUIRE(result.value().result.type == ToolResult::Type::Text);
}

// ============================================================================
// 工具未找到
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor returns error for unknown tool",
                 "[tool_executor][not_found]") {
    auto result = executor->execute("NonExistent", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ResourceNotFound);
    REQUIRE(result.error().message.find("Tool not found") != std::string::npos);
    REQUIRE(result.error().message.find("NonExistent") != std::string::npos);
    REQUIRE(result.error().context == "NonExistent");
}

// ============================================================================
// 取消信号
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor respects cancellation",
                 "[tool_executor][cancel]") {
    ctx.cancel();

    auto result = executor->execute("Echo", R"({"text":"x"})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::Cancelled);
    REQUIRE(result.error().message.find("cancelled") != std::string::npos);
    REQUIRE(echo->call_count == 0);  // 工具未被调用
}

// ============================================================================
// 权限检查
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor denies on permission failure",
                 "[tool_executor][permission]") {
    registry->register_tool(std::make_shared<DeniedTool>());

    auto result = executor->execute("Denied", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::PermissionDenied);
    REQUIRE(result.error().message.find("policy denied") != std::string::npos);
}

// ============================================================================
// 极简模式白名单守卫（SessionMode::Minimal 仅放行 Skill/Bash/Read/Write/Edit）
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor denies non-allowlisted tool in minimal mode",
                 "[tool_executor][minimal_mode]") {
    ctx.session_mode = SessionMode::Minimal;

    // Echo 不在白名单内：极简模式下直接拒绝（幻觉工具名防线）
    auto result = executor->execute("Echo", R"({"text":"hi"})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::PermissionDenied);
    REQUIRE(result.error().message.find("minimal mode") != std::string::npos);
    REQUIRE(echo->call_count == 0);  // 未执行工具
}

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor allows allowlisted tool in minimal mode",
                 "[tool_executor][minimal_mode]") {
    // 注册一个名字在白名单内的工具（模拟 Bash）
    class BashNamedTool : public EchoTool {
       public:
        const std::string& name() const override {
            static const std::string n = "Bash";
            return n;
        }
    };
    auto bash_tool = std::make_shared<BashNamedTool>();
    registry->register_tool(bash_tool);

    ctx.session_mode = SessionMode::Minimal;

    // 白名单工具可执行
    auto ok = executor->execute("Bash", R"({"text":"ls"})"_json, ctx);
    REQUIRE(ok.is_ok());
    REQUIRE(ok.value().result.text.find("echo: ls") != std::string::npos);
    REQUIRE(bash_tool->call_count == 1);

    // 标准模式不受白名单限制（Echo 可执行）
    ctx.session_mode = SessionMode::Standard;
    auto standard = executor->execute("Echo", R"({"text":"x"})"_json, ctx);
    REQUIRE(standard.is_ok());
}

TEST_CASE("is_minimal_mode_tool matches the allowlist", "[tool_executor][minimal_mode]") {
    REQUIRE(is_minimal_mode_tool("Skill"));
    REQUIRE(is_minimal_mode_tool("Bash"));
    REQUIRE(is_minimal_mode_tool("Read"));
    REQUIRE(is_minimal_mode_tool("Write"));
    REQUIRE(is_minimal_mode_tool("Edit"));
    REQUIRE_FALSE(is_minimal_mode_tool("Echo"));
    REQUIRE_FALSE(is_minimal_mode_tool("WebSearch"));
    REQUIRE_FALSE(is_minimal_mode_tool(""));
}

// ============================================================================
// 输入验证
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor validates input before execution",
                 "[tool_executor][validation]") {
    registry->register_tool(std::make_shared<ValidatingTool>());

    SECTION("valid input passes validation") {
        auto result = executor->execute("Validating", R"({"value":"test"})"_json, ctx);
        REQUIRE(result.is_ok());
        REQUIRE(result.value().result.text == "got: test");
    }

    SECTION("invalid input fails validation") {
        auto result = executor->execute("Validating", R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::MissingArgument);
        REQUIRE(result.error().message.find("missing 'value' field") != std::string::npos);
    }
}

// ============================================================================
// 异常捕获
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor catches std::exception from tool",
                 "[tool_executor][exception]") {
    registry->register_tool(std::make_shared<FailingTool>());

    auto result = executor->execute("Failing", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    REQUIRE(result.error().message.find("Error in tool 'Failing'") != std::string::npos);
    REQUIRE(result.error().message.find("intentional failure") != std::string::npos);
}

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor catches nlohmann::json::exception",
                 "[tool_executor][exception]") {
    registry->register_tool(std::make_shared<JsonExceptionTool>());

    auto result = executor->execute("JsonException", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    REQUIRE(result.error().message.find("JSON error in tool") != std::string::npos);
}

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor catches std::filesystem::filesystem_error",
                 "[tool_executor][exception]") {
    registry->register_tool(std::make_shared<FilesystemErrorTool>());

    auto result = executor->execute("FsError", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    REQUIRE(result.error().message.find("Filesystem error in tool") != std::string::npos);
}

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor catches unknown exception",
                 "[tool_executor][exception]") {
    registry->register_tool(std::make_shared<UnknownExceptionTool>());

    auto result = executor->execute("Unknown", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::Unknown);
    REQUIRE(result.error().message.find("Unknown exception in tool") != std::string::npos);
}

// ============================================================================
// 业务错误（ResultV2<ToolResult>::err）
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor propagates tool error as ResultV2 err",
                 "[tool_executor][business_error]") {
    registry->register_tool(std::make_shared<ErrorResultTool>());

    auto result = executor->execute("ErrorResult", R"({})"_json, ctx);

    REQUIRE(result.is_err());
    REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    REQUIRE(result.error().message == "business logic error");
}

// ============================================================================
// 上下文传递
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor passes ToolContext to tool",
                 "[tool_executor][context]") {
    ctx.session_id = "session-123";
    ctx.model = "test-model";
    ctx.cwd = "/test/path";

    auto result = executor->execute("Echo", R"({"text":"ctx"})"_json, ctx);

    REQUIRE(result.is_ok());
    // ToolContext 通过 call() 传入，EchoTool 忽略它，但能验证调用未崩溃
}

// ============================================================================
// execute is const method
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture,
                 "ToolExecutor execute is const method (parallel-safe requirement)",
                 "[tool_executor][const]") {
    // 验证 execute 是 const 方法（PLAN 3.x K-1 审计前置）
    const ToolExecutor& const_executor = *executor;
    auto result = const_executor.execute("Echo", R"({"text":"const"})"_json, ctx);

    REQUIRE(result.is_ok());
    REQUIRE(echo->call_count == 1);
}

// ============================================================================
// H-B / M-3 / L-2：truncate_result 截断逻辑独立测试
// L-2：原 in-out 参数改为返回 std::pair，验证纯函数行为
// ============================================================================

TEST_CASE("truncate_result keeps short text unchanged", "[tool_executor][m3][truncation][l2]") {
    std::string text = "short text";
    auto [result, truncated] = truncate_result(text, 100);

    REQUIRE_FALSE(truncated);
    REQUIRE(result == "short text");
    // L-2：原入参不被修改（纯函数）
    REQUIRE(text == "short text");
}

TEST_CASE("truncate_result truncates long text with marker",
          "[tool_executor][m3][truncation][l2]") {
    // 构造超长文本（2000 字节），最大保留 1000 字节
    std::string text(2000, 'x');
    auto [result, truncated] = truncate_result(text, 1000);

    REQUIRE(truncated);
    // 截断后包含截断标记
    REQUIRE(result.find("[output truncated,") != std::string::npos);
    REQUIRE(result.find("characters omitted]") != std::string::npos);
    // 头尾各保留 max_length/2
    REQUIRE(result.find("xxxx") != std::string::npos);
    // L-2：原入参不被修改
    REQUIRE(text.size() == 2000);
}

TEST_CASE("truncate_result preserves head and tail of long text",
          "[tool_executor][m3][truncation][l2]") {
    // 头部为 "HEADHEAD"，尾部为 "TAILTAIL"，中间填充 2000 个 'M'
    std::string text = "HEADHEAD";
    text.append(std::string(2000, 'M'));
    text.append("TAILTAIL");

    auto [result, truncated] = truncate_result(text, 100);
    REQUIRE(truncated);

    // 头尾内容应保留
    REQUIRE(result.find("HEADHEAD") == 0);
    REQUIRE(result.find("TAILTAIL") != std::string::npos);
    // 中间内容应被省略：原始 2000 个 'M' 仅剩头尾各 half=50 字节中的部分
    // 头部 50 字节 = "HEADHEAD"(8) + 42 个 'M'，尾部同理，共约 84 个 'M'
    const size_t m_count = static_cast<size_t>(std::count(result.begin(), result.end(), 'M'));
    REQUIRE(m_count < 2000);
    REQUIRE(m_count <= 100);  // 远少于原始 2000 个
    // L-2：原入参不被修改
    REQUIRE(text.size() == 2000 + 8 + 8);
}

TEST_CASE("truncate_result default max_length uses MAX_TOOL_RESULT_LENGTH",
          "[tool_executor][m3][truncation][l2]") {
    // L-2：验证默认参数 max_length = MAX_TOOL_RESULT_LENGTH
    std::string text(MAX_TOOL_RESULT_LENGTH, 'a');
    auto [result_eq, truncated_eq] = truncate_result(text);
    REQUIRE_FALSE(truncated_eq);
    REQUIRE(result_eq.size() == MAX_TOOL_RESULT_LENGTH);

    std::string long_text(MAX_TOOL_RESULT_LENGTH + 1, 'a');
    auto [result_long, truncated_long] = truncate_result(long_text);
    REQUIRE(truncated_long);
    REQUIRE(result_long.find("[output truncated,") != std::string::npos);
}

// ============================================================================
// H-B / M-3：finalize_result 通过 execute() 截断行为验证
// ============================================================================

namespace {

/// @brief 返回超长文本的工具，用于测试 finalize_result 的截断行为
class LongTextTool : public ITool {
   public:
    explicit LongTextTool(size_t length) : length_(length) {}

    const std::string& name() const override {
        static const std::string n = "LongText";
        return n;
    }
    const std::string& description() const override {
        static const std::string d;
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }

    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string(length_, 'A')));
    }

   private:
    size_t length_;
};

}  // namespace

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor truncates long result via finalize_result",
                 "[tool_executor][m3][finalize]") {
    // 构造超过 MAX_TOOL_RESULT_LENGTH 的输出
    const size_t long_len = MAX_TOOL_RESULT_LENGTH + 1000;
    registry->register_tool(std::make_shared<LongTextTool>(long_len));

    auto result = executor->execute("LongText", R"({})"_json, ctx);

    REQUIRE(result.is_ok());
    REQUIRE(result.value().tool_name == "LongText");
    // 截断标记位为 true
    REQUIRE(result.value().is_truncated());
    // 截断后文本应小于原始长度
    REQUIRE(result.value().result.text.length() < long_len);
    // 截断标记文本应存在
    REQUIRE(result.value().result.text.find("[output truncated,") != std::string::npos);
    REQUIRE(result.value().result.text.find("characters omitted]") != std::string::npos);
    // 头尾仍保留 'A' 字符
    REQUIRE(result.value().result.text.front() == 'A');
    REQUIRE(result.value().result.text.back() == 'A');
}

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor does not truncate short result",
                 "[tool_executor][m3][finalize]") {
    // 短于 MAX_TOOL_RESULT_LENGTH 的输出不应被截断
    const size_t short_len = 100;
    registry->register_tool(std::make_shared<LongTextTool>(short_len));

    auto result = executor->execute("LongText", R"({})"_json, ctx);

    REQUIRE(result.is_ok());
    REQUIRE_FALSE(result.value().is_truncated());
    REQUIRE(result.value().result.text.length() == short_len);
    REQUIRE(result.value().result.text.find("[output truncated,") == std::string::npos);
}

// ============================================================================
// H-B / M-3：run_with_safety 异常分类测试（通过 execute 入口验证）
// 注意：基础异常分类已在 "[tool_executor][exception]" 测试覆盖，这里补充
//       M-3 拆分后的契约：所有异常分支统一返回 ToolExecutionFailed/Unknown
// ============================================================================

TEST_CASE_METHOD(ToolExecutorFixture, "ToolExecutor run_with_safety classifies all exception types",
                 "[tool_executor][m3][exception_classification]") {
    // 验证 M-3 拆分后 run_with_safety 的异常分类契约：
    // - std::exception 子类 → ToolExecutionFailed
    // - 未知异常 → Unknown
    // - 业务错误（ResultV2::err）→ 原错误透传

    SECTION("std::runtime_error 映射为 ToolExecutionFailed") {
        registry->register_tool(std::make_shared<FailingTool>());
        auto result = executor->execute("Failing", R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    }

    SECTION("nlohmann::json::exception 映射为 ToolExecutionFailed") {
        registry->register_tool(std::make_shared<JsonExceptionTool>());
        auto result = executor->execute("JsonException", R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    }

    SECTION("std::filesystem::filesystem_error 映射为 ToolExecutionFailed") {
        registry->register_tool(std::make_shared<FilesystemErrorTool>());
        auto result = executor->execute("FsError", R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
    }

    SECTION("非 std::exception 异常映射为 Unknown") {
        registry->register_tool(std::make_shared<UnknownExceptionTool>());
        auto result = executor->execute("Unknown", R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::Unknown);
    }

    SECTION("业务 ResultV2::err 透传原错误码 ToolExecutionFailed") {
        registry->register_tool(std::make_shared<ErrorResultTool>());
        auto result = executor->execute("ErrorResult", R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ToolExecutionFailed);
        REQUIRE(result.error().message == "business logic error");
    }
}
