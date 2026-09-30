/**
 * @file test_tool_registry.cpp
 * @brief ToolRegistry 单元测试
 * @details 覆盖 register_tool/find_by_name/get_all_tools/get_all_schemas/exists/size
 */

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <type_traits>

#include "agent/tool/registry.h"
#include "agent/tool/itool.h"
#include "agent/tool/result.h"

using namespace agent;
using namespace agent::tool;

namespace {

class StubTool : public ITool {
   public:
    StubTool(std::string n, std::string d = "", nlohmann::json schema = {{"type", "object"}})
        : name_(std::move(n)), desc_(std::move(d)), schema_(std::move(schema)) {}

    const std::string& name() const override { return name_; }
    const std::string& description() const override { return desc_; }
    const std::string& prompt() const override {
        static const std::string p;
        return p;
    }
    nlohmann::json input_schema() const override { return schema_; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("ok")));
    }

   private:
    std::string name_;
    std::string desc_;
    nlohmann::json schema_;
};

}  // namespace

// ============================================================================
// register_tool
// ============================================================================

TEST_CASE("ToolRegistry register_tool adds tool", "[tool_registry][register]") {
    ToolRegistry registry;
    REQUIRE(registry.size() == 0);

    registry.register_tool(std::make_shared<StubTool>("ToolA"));
    REQUIRE(registry.size() == 1);

    registry.register_tool(std::make_shared<StubTool>("ToolB"));
    REQUIRE(registry.size() == 2);
}

TEST_CASE("ToolRegistry register_tool ignores null", "[tool_registry][register]") {
    ToolRegistry registry;
    registry.register_tool(nullptr);
    REQUIRE(registry.size() == 0);
}

TEST_CASE("ToolRegistry register_tool allows duplicate name - last wins",
          "[tool_registry][register]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("Dup"));
    registry.register_tool(std::make_shared<StubTool>("Dup"));

    // 当前实现允许同名注册，name_index_ 后者覆盖前者
    REQUIRE(registry.size() == 2);  // tools_ 列表有两个
    REQUIRE(registry.exists("Dup"));
    auto found = registry.find_by_name("Dup");
    REQUIRE(found != nullptr);
}

// ============================================================================
// find_by_name
// ============================================================================

TEST_CASE("ToolRegistry find_by_name returns registered tool", "[tool_registry][find]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("Findable"));

    auto tool = registry.find_by_name("Findable");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->name() == "Findable");
}

TEST_CASE("ToolRegistry find_by_name returns nullptr for unknown", "[tool_registry][find]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A"));

    auto tool = registry.find_by_name("NonExistent");
    REQUIRE(tool == nullptr);
}

TEST_CASE("ToolRegistry find_by_name on empty registry returns nullptr", "[tool_registry][find]") {
    ToolRegistry registry;
    REQUIRE(registry.find_by_name("Anything") == nullptr);
}

// ============================================================================
// get_all_tools
// ============================================================================

TEST_CASE("ToolRegistry get_all_tools returns all registered", "[tool_registry][all]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A"));
    registry.register_tool(std::make_shared<StubTool>("B"));
    registry.register_tool(std::make_shared<StubTool>("C"));

    auto tools = registry.get_all_tools();
    REQUIRE(tools.size() == 3);
    REQUIRE(tools[0]->name() == "A");
    REQUIRE(tools[1]->name() == "B");
    REQUIRE(tools[2]->name() == "C");
}

TEST_CASE("ToolRegistry get_all_tools on empty returns empty vector", "[tool_registry][all]") {
    ToolRegistry registry;
    auto tools = registry.get_all_tools();
    REQUIRE(tools.empty());
}

// ============================================================================
// get_all_schemas
// ============================================================================

TEST_CASE("ToolRegistry get_all_schemas returns JSON array", "[tool_registry][schema]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>(
        "ToolA", "Description A",
        R"({"type":"object","properties":{"x":{"type":"string"}}})"_json));

    auto schemas = registry.get_all_schemas();

    REQUIRE(schemas.is_array());
    REQUIRE(schemas.size() == 1);
    REQUIRE(schemas[0]["name"] == "ToolA");
    REQUIRE(schemas[0]["description"] == "Description A");
    REQUIRE(schemas[0]["input_schema"]["type"] == "object");
}

TEST_CASE("ToolRegistry get_all_schemas for multiple tools", "[tool_registry][schema]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A", "desc A"));
    registry.register_tool(std::make_shared<StubTool>("B", "desc B"));

    auto schemas = registry.get_all_schemas();

    REQUIRE(schemas.size() == 2);
    REQUIRE(schemas[0]["name"] == "A");
    REQUIRE(schemas[1]["name"] == "B");
}

TEST_CASE("ToolRegistry get_all_schemas on empty returns empty array", "[tool_registry][schema]") {
    ToolRegistry registry;
    auto schemas = registry.get_all_schemas();
    REQUIRE(schemas.is_array());
    REQUIRE(schemas.empty());
}

TEST_CASE("ToolRegistry get_schemas_by_names filters by whitelist",
          "[tool_registry][schema][filter]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A", "desc A"));
    registry.register_tool(std::make_shared<StubTool>("B", "desc B"));
    registry.register_tool(std::make_shared<StubTool>("C", "desc C"));

    auto schemas = registry.get_schemas_by_names({"B", "C"});
    REQUIRE(schemas.is_array());
    REQUIRE(schemas.size() == 2);
    REQUIRE(schemas[0]["name"] == "B");
    REQUIRE(schemas[1]["name"] == "C");
}

TEST_CASE("ToolRegistry get_schemas_by_names ignores unknown names",
          "[tool_registry][schema][filter]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A", "desc A"));

    auto schemas = registry.get_schemas_by_names({"A", "Nope", ""});
    REQUIRE(schemas.size() == 1);
    REQUIRE(schemas[0]["name"] == "A");
}

TEST_CASE("ToolRegistry get_schemas_by_names with empty whitelist returns empty",
          "[tool_registry][schema][filter]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A", "desc A"));

    auto schemas = registry.get_schemas_by_names({});
    REQUIRE(schemas.is_array());
    REQUIRE(schemas.empty());
}

// ============================================================================
// exists & size
// ============================================================================

TEST_CASE("ToolRegistry exists returns true for registered", "[tool_registry][exists]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("Exists"));

    REQUIRE(registry.exists("Exists"));
    REQUIRE_FALSE(registry.exists("DoesNotExist"));
}

TEST_CASE("ToolRegistry size returns correct count", "[tool_registry][size]") {
    ToolRegistry registry;
    REQUIRE(registry.size() == 0);

    registry.register_tool(std::make_shared<StubTool>("A"));
    REQUIRE(registry.size() == 1);

    registry.register_tool(std::make_shared<StubTool>("B"));
    registry.register_tool(std::make_shared<StubTool>("C"));
    REQUIRE(registry.size() == 3);
}

// ============================================================================
// H-B / M-5：ITool ISP 拆分契约测试
// 验证 IToolMetadata / IToolGuard / IToolCallable 可被独立实现与独立消费，
// 不需要依赖完整的 ITool 胖接口。
// ============================================================================

namespace {

/// @brief 仅实现 IToolCallable 的 Stub（M-5 ISP）
/// @details 验证消费方可以只依赖执行能力，不需要元信息或 Guard。
class StubToolCallable : public IToolCallable {
   public:
    mutable int call_count = 0;
    mutable std::string last_input;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& /*ctx*/
    ) const override {
        ++call_count;
        last_input = input.value("text", "");
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("callable: ") + last_input));
    }
};

/// @brief 仅实现 IToolMetadata 的 Stub（M-5 ISP）
/// @details 验证 UI/registry 列举场景可只依赖元信息，不需要执行能力。
class StubToolMetadata : public IToolMetadata {
   public:
    const std::string& name() const override {
        static const std::string n = "StubMeta";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "metadata-only stub";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "prompt";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {{"x", {{"type", "string"}}}}}};
    }
};

/// @brief 仅实现 IToolGuard 的 Stub（M-5 ISP）
/// @details 验证审计/拦截层可只依赖 Guard，不需要执行能力或元信息。
class StubToolGuard : public IToolGuard {
   public:
    mutable int perm_count = 0;
    mutable int valid_count = 0;

    PermissionResult check_permissions(const nlohmann::json& input, const ToolContext& /*ctx*/
    ) const override {
        ++perm_count;
        if (!input.contains("forbidden")) {
            return PermissionResult::ok();
        }
        return PermissionResult::err(Error::Code::PermissionDenied, "forbidden field present");
    }

    ValidationResult validate_input(const nlohmann::json& input, const ToolContext& /*ctx*/
    ) const override {
        ++valid_count;
        if (!input.contains("required_field")) {
            return ValidationResult::err(Error::Code::MissingArgument, "required_field missing");
        }
        return ValidationResult::ok();
    }
};

/// @brief 自由函数：只依赖 IToolCallable&（M-5 ISP 契约）
/// @details 若 IToolCallable 不能被独立实现/消费，此函数无法编译。
///          等价于 ToolExecutor::run_with_safety 的依赖最小化契约。
static ResultV2<ToolResult> invoke_callable(IToolCallable& callable, const nlohmann::json& input,
                                            const ToolContext& ctx) {
    return callable.call(input, ctx);
}

/// @brief 自由函数：只依赖 IToolMetadata&（M-5 ISP 契约）
/// @details 模拟 UI 列举场景，只需要 name/description/schema。
static nlohmann::json describe_metadata(IToolMetadata& meta) {
    return {
        {"name", meta.name()},
        {"description", meta.description()},
        {"input_schema", meta.input_schema()},
    };
}

/// @brief 自由函数：只依赖 IToolGuard&（M-5 ISP 契约）
/// @details 模拟审计层场景，只需要权限/输入校验。
static ResultV2<void> audit_guard(IToolGuard& guard, const nlohmann::json& input,
                                  const ToolContext& ctx) {
    auto perm = guard.check_permissions(input, ctx);
    if (perm.is_err()) return perm;
    return guard.validate_input(input, ctx);
}

}  // namespace

TEST_CASE("IToolCallable can be implemented and consumed independently",
          "[tool_registry][m5][isp]") {
    // M-5 ISP 契约：仅实现 IToolCallable 的类可以被只依赖 IToolCallable& 的代码消费
    StubToolCallable callable;
    ToolContext ctx;
    ctx.session_id = "test";

    SECTION("invoke_callable 接收 IToolCallable& 并调用 call") {
        auto result = invoke_callable(callable, R"({"text":"hi"})"_json, ctx);
        REQUIRE(result.is_ok());
        REQUIRE(result.value().text == "callable: hi");
        REQUIRE(callable.call_count == 1);
        REQUIRE(callable.last_input == "hi");
    }

    SECTION("多次调用累加 call_count") {
        invoke_callable(callable, R"({"text":"a"})"_json, ctx);
        invoke_callable(callable, R"({"text":"b"})"_json, ctx);
        invoke_callable(callable, R"({"text":"c"})"_json, ctx);
        REQUIRE(callable.call_count == 3);
    }
}

TEST_CASE("IToolMetadata can be implemented and consumed independently",
          "[tool_registry][m5][isp]") {
    // M-5 ISP 契约：仅实现 IToolMetadata 的类可以被只依赖 IToolMetadata& 的代码消费
    StubToolMetadata meta;

    SECTION("describe_metadata 返回 name/description/schema") {
        auto desc = describe_metadata(meta);
        REQUIRE(desc["name"] == "StubMeta");
        REQUIRE(desc["description"] == "metadata-only stub");
        REQUIRE(desc["input_schema"]["type"] == "object");
    }

    SECTION("多次调用 name() 返回同一引用") {
        const auto& n1 = meta.name();
        const auto& n2 = meta.name();
        REQUIRE(&n1 == &n2);
    }
}

TEST_CASE("IToolGuard can be implemented and consumed independently", "[tool_registry][m5][isp]") {
    // M-5 ISP 契约：仅实现 IToolGuard 的类可以被只依赖 IToolGuard& 的代码消费
    StubToolGuard guard;
    ToolContext ctx;

    SECTION("合法输入通过 guard") {
        auto result = audit_guard(guard, R"({"required_field":"x"})"_json, ctx);
        REQUIRE(result.is_ok());
        REQUIRE(guard.perm_count == 1);
        REQUIRE(guard.valid_count == 1);
    }

    SECTION("缺必填字段被 validate_input 拒绝") {
        auto result = audit_guard(guard, R"({})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::MissingArgument);
        REQUIRE(guard.perm_count == 1);
        REQUIRE(guard.valid_count == 1);
    }

    SECTION("含 forbidden 字段被 check_permissions 拒绝") {
        auto result = audit_guard(guard, R"({"forbidden":true,"required_field":"x"})"_json, ctx);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::PermissionDenied);
        REQUIRE(guard.perm_count == 1);
        // check_permissions 失败时不调用 validate_input
        REQUIRE(guard.valid_count == 0);
    }
}

TEST_CASE("ITool combines Metadata/Guard/Callable (backward compat)", "[tool_registry][m5][isp]") {
    // M-5 ISP 契约：ITool 继承三者，现有工具实现 ITool 即可获得三个角色
    // 已被 StubTool（本文件顶部）覆盖，这里通过 static_assert 补充编译期契约
    static_assert(std::is_base_of_v<IToolMetadata, ITool>,
                  "ITool must inherit IToolMetadata (M-5 ISP)");
    static_assert(std::is_base_of_v<IToolGuard, ITool>, "ITool must inherit IToolGuard (M-5 ISP)");
    static_assert(std::is_base_of_v<IToolCallable, ITool>,
                  "ITool must inherit IToolCallable (M-5 ISP)");

    // 验证 ITool* 可隐式转换为三个子接口指针（多向上转换）
    StubTool tool("Combo");
    ITool* itool = &tool;

    IToolMetadata* meta_ptr = itool;
    IToolGuard* guard_ptr = itool;
    IToolCallable* callable_ptr = itool;

    REQUIRE(meta_ptr->name() == "Combo");
    REQUIRE(guard_ptr != nullptr);
    REQUIRE(callable_ptr != nullptr);

    // StubTool 默认 check_permissions/validate_input 通过，call 返回 ok
    ToolContext ctx;
    REQUIRE(guard_ptr->check_permissions(R"({})"_json, ctx).is_ok());
    REQUIRE(callable_ptr->call(R"({})"_json, ctx).is_ok());
}
