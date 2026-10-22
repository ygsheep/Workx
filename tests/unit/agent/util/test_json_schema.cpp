/**
 * @file test_json_schema.cpp
 * @brief JSON Schema 轻量校验单元测试（Issue #80）
 */

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <string>

#include "agent/tool/context.h"
#include "agent/tool/executor.h"
#include "agent/tool/itool.h"
#include "agent/tool/registry.h"
#include "agent/util/json_schema.h"
#include "core/utils/error.h"

using namespace agent;
using namespace agent::util;
using nlohmann::json;

// ============================================================================
// validate_schema 纯函数测试
// ============================================================================

TEST_CASE("validate_schema 非法类型被拦截", "[json_schema][type]") {
    json schema = {
        {"type", "object"},
        {"properties", {{"file_path", {{"type", "string"}}}}},
        {"required", {"file_path"}},
    };
    // file_path 传数字而非字符串
    json input = {{"file_path", 123}};
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.errors.size() == 1);
    REQUIRE(res.errors[0].expected == "string");
    // json_type_name 对整数返回更精确的 "integer"（而非 "number"）
    REQUIRE(res.errors[0].actual == "integer");
}

TEST_CASE("validate_schema 缺必填字段被拦截", "[json_schema][required]") {
    json schema = {
        {"type", "object"},
        {"properties", {{"content", {{"type", "string"}}}}},
        {"required", {"content"}},
    };
    json input = json::object();  // 空对象，缺 content
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.errors.size() == 1);
    REQUIRE(res.errors[0].is_missing == true);
    REQUIRE(res.errors[0].param == "/content");
}

TEST_CASE("validate_schema 越界枚举被拦截", "[json_schema][enum]") {
    json schema = {
        {"type", "object"},
        {"properties", {{"mode", {{"type", "string"}, {"enum", {"read", "write"}}}}}},
        {"required", {"mode"}},
    };
    json input = {{"mode", "delete"}};  // 不在枚举内
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.errors[0].param == "/mode");
}

TEST_CASE("validate_schema 嵌套 object 校验", "[json_schema][nested]") {
    json schema = {
        {"type", "object"},
        {"properties",
         {
             {"config",
              {
                  {"type", "object"},
                  {"properties", {{"timeout", {{"type", "integer"}}}}},
                  {"required", {"timeout"}},
              }},
         }},
        {"required", {"config"}},
    };
    // config.timeout 缺失
    json input = {{"config", json::object()}};
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.errors[0].param == "/config/timeout");
    REQUIRE(res.errors[0].is_missing == true);
}

TEST_CASE("validate_schema 合法输入通过", "[json_schema][ok]") {
    json schema = {
        {"type", "object"},
        {"properties",
         {
             {"file_path", {{"type", "string"}}},
             {"content", {{"type", "string"}}},
         }},
        {"required", {"file_path", "content"}},
    };
    json input = {{"file_path", "/a/b.txt"}, {"content", "hello"}};
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE(res.ok);
    REQUIRE(res.errors.empty());
}

TEST_CASE("validate_schema 空 schema 不校验", "[json_schema][empty]") {
    json schema = json::object();  // 无约束
    json input = {{"anything", 123}};
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE(res.ok);
}

TEST_CASE("validate_schema 顶层非 object 输入被拦截", "[json_schema][toplevel]") {
    json schema = {{"type", "object"}};
    json input = json::array({1, 2, 3});
    auto res = validate_schema(schema, input, "TestTool");
    REQUIRE_FALSE(res.ok);
    REQUIRE(res.errors[0].expected == "object");
}

// ============================================================================
// ToolExecutor 集成：只声明 schema、不写 validate_input 的工具应被拦截
// ============================================================================

namespace {

/// @brief 只声明 schema、不覆盖 validate_input 的测试工具
/// @details 直接对应 issue #80 验收标准：默认 validate_input 返回 ok，
///          靠 ToolExecutor 的统一 schema 校验兜底拦截非法参数。
class SchemaOnlyTool : public agent::tool::ITool {
   public:
    const std::string& name() const override {
        static const std::string n = "SchemaOnlyTool";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "test tool with schema only";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "test";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {{"path", {{"type", "string"}}}}},
            {"required", {"path"}},
        };
    }
    ResultV2<agent::tool::ToolResult> call(const nlohmann::json&,
                                           const agent::tool::ToolContext&) const override {
        agent::tool::ToolResult r;
        r.text = "ok";
        return ResultV2<agent::tool::ToolResult>::ok(std::move(r));
    }
};

}  // namespace

TEST_CASE("ToolExecutor 拦截只声明 schema 工具的非法参数", "[json_schema][executor]") {
    auto registry = std::make_shared<agent::tool::ToolRegistry>();
    registry->register_tool(std::make_shared<SchemaOnlyTool>());

    agent::tool::ToolExecutor executor(registry);
    agent::tool::ToolContext ctx;

    // 缺必填 path：应被统一 schema 校验拦截，返回 MissingArgument
    json bad_input = json::object();
    auto res = executor.execute("SchemaOnlyTool", bad_input, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::MissingArgument);

    // 非法类型：path 传数字
    json type_bad = {{"path", 42}};
    auto res2 = executor.execute("SchemaOnlyTool", type_bad, ctx);
    REQUIRE(res2.is_err());
    REQUIRE(res2.error().code == Error::Code::InvalidInput);

    // 合法输入：应通过
    json good = {{"path", "/tmp/x"}};
    auto res3 = executor.execute("SchemaOnlyTool", good, ctx);
    REQUIRE(res3.is_ok());
}
