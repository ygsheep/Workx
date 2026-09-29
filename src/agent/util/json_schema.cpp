/**
 * @file json_schema.cpp
 * @brief JSON Schema 轻量验证实现（Issue #80）
 */

#include "agent/util/json_schema.h"

#include <regex>
#include <sstream>

namespace agent::util {

namespace {

// 前向声明：validate_value 与 validate_properties 互相递归调用
void validate_value(const nlohmann::json& schema,
                    const nlohmann::json& value,
                    const std::string& tool_name,
                    const std::string& param_path,
                    int depth,
                    std::vector<SchemaError>& out);

void validate_properties(const nlohmann::json& schema,
                         const nlohmann::json& value,
                         const std::string& tool_name,
                         const std::string& param_path,
                         int depth,
                         std::vector<SchemaError>& out);

/// @brief 将 nlohmann::json 值类型转为可读字符串（用于错误信息）
std::string json_type_name(const nlohmann::json& j) {
    if (j.is_null()) return "null";
    if (j.is_boolean()) return "boolean";
    if (j.is_number_integer()) return "integer";
    if (j.is_number_unsigned()) return "integer";
    if (j.is_number_float()) return "number";
    if (j.is_string()) return "string";
    if (j.is_array()) return "array";
    if (j.is_object()) return "object";
    return "unknown";
}

/// @brief 将输入值简化为可读描述（用于错误信息 actual 字段）
std::string describe_actual(const nlohmann::json& j) {
    if (j.is_string()) return "\"" + j.get<std::string>() + "\"";
    if (j.is_null()) return "null";
    if (j.is_boolean()) return j.get<bool>() ? "true" : "false";
    if (j.is_number()) {
        std::ostringstream oss;
        oss << j.get<double>();
        return oss.str();
    }
    // 复合类型只描述类型，避免 dump 大对象
    return json_type_name(j);
}

/// @brief 类型名是否匹配 schema 的 "type" 声明
/// @details integer 与 number 的兼容：schema 声明 "number" 时接受 integer；
///          声明 "integer" 时仅接受整数。
bool type_matches(const std::string& expected, const nlohmann::json& value) {
    if (expected == "string") return value.is_string();
    if (expected == "number") return value.is_number();
    if (expected == "integer") return value.is_number_integer() || value.is_number_unsigned();
    if (expected == "boolean") return value.is_boolean();
    if (expected == "array") return value.is_array();
    if (expected == "object") return value.is_object();
    if (expected == "null") return value.is_null();
    return false;  // 未知类型关键字：不匹配（保守拒绝）
}

/// @brief 校验上下文：承载一趟校验中不变的参数，避免逐函数重复传递
struct ValidateCtx {
    const std::string* tool_name;
    const std::string* param_path;
    std::vector<SchemaError>* out;
};

/// @brief 构造并追加一条 SchemaError（消除各校验分支中重复的构造代码）
void add_error(const ValidateCtx& c, const std::string& expected,
               const std::string& actual, const std::string& hint,
               bool is_missing = false) {
    SchemaError e;
    e.tool = *c.tool_name;
    e.param = *c.param_path;
    e.expected = expected;
    e.actual = actual;
    e.hint = hint;
    e.is_missing = is_missing;
    c.out->push_back(std::move(e));
}

/// @brief 校验 "enum" 约束
void validate_enum(const nlohmann::json& schema, const nlohmann::json& value,
                   const ValidateCtx& c) {
    if (!schema.contains("enum") || !schema["enum"].is_array()) return;
    for (const auto& opt : schema["enum"]) {
        if (opt == value) return;
    }
    add_error(c, "one of " + schema["enum"].dump(), describe_actual(value), "枚举值越界");
}

/// @brief 校验数值范围约束（minimum / maximum）
void validate_range(const nlohmann::json& schema, const nlohmann::json& value,
                    const ValidateCtx& c) {
    if (!value.is_number()) return;
    if (schema.contains("minimum") && schema["minimum"].is_number() &&
        value.get<double>() < schema["minimum"].get<double>()) {
        add_error(c, ">= " + schema["minimum"].dump(), describe_actual(value), "数值小于最小值");
    }
    if (schema.contains("maximum") && schema["maximum"].is_number() &&
        value.get<double>() > schema["maximum"].get<double>()) {
        add_error(c, "<= " + schema["maximum"].dump(), describe_actual(value), "数值大于最大值");
    }
}

/// @brief 校验字符串长度约束（minLength / maxLength）
void validate_length(const nlohmann::json& schema, const nlohmann::json& value,
                     const ValidateCtx& c) {
    if (!value.is_string()) return;
    const auto len = static_cast<long long>(value.get<std::string>().length());
    if (schema.contains("minLength") && schema["minLength"].is_number_integer() &&
        len < schema["minLength"].get<long long>()) {
        add_error(c, "length >= " + std::to_string(schema["minLength"].get<long long>()),
                  "length " + std::to_string(len), "字符串长度不足");
    }
    if (schema.contains("maxLength") && schema["maxLength"].is_number_integer() &&
        len > schema["maxLength"].get<long long>()) {
        add_error(c, "length <= " + std::to_string(schema["maxLength"].get<long long>()),
                  "length " + std::to_string(len), "字符串长度超限");
    }
}

/// @brief 校验字符串 pattern 约束（非法正则降级跳过，不阻断）
void validate_pattern(const nlohmann::json& schema, const nlohmann::json& value,
                      const ValidateCtx& c) {
    if (!value.is_string()) return;
    if (!schema.contains("pattern") || !schema["pattern"].is_string()) return;
    try {
        std::regex re(schema["pattern"].get<std::string>());
        if (!std::regex_match(value.get<std::string>(), re)) {
            add_error(c, "match /" + schema["pattern"].get<std::string>() + "/",
                      describe_actual(value), "字符串不匹配 pattern");
        }
    } catch (const std::regex_error&) {
        // 非法 pattern：跳过该项校验（降级，不阻断）
    }
}

/// @brief 校验 array 元素（浅层，受 depth 限制）
void validate_items(const nlohmann::json& schema, const nlohmann::json& value,
                    const std::string& tool_name, const std::string& param_path,
                    int depth, std::vector<SchemaError>& out) {
    if (!value.is_array() || depth <= 0) return;
    if (!schema.contains("items") || !schema["items"].is_object()) return;
    size_t idx = 0;
    for (const auto& item : value) {
        validate_value(schema["items"], item, tool_name,
                       param_path + "/" + std::to_string(idx), depth - 1, out);
        ++idx;
    }
}

/// @brief 检查单值是否符合其 schema 约束
/// @details 仅做调度：按 type → enum → range → length → pattern → items → 嵌套 object
///          依次分派到各校验函数，各约束细节见对应函数。
/// @param param_path JSON Pointer 风格路径（如 "/content"）
/// @param out 错误收集
void validate_value(const nlohmann::json& schema,
                    const nlohmann::json& value,
                    const std::string& tool_name,
                    const std::string& param_path,
                    int depth,
                    std::vector<SchemaError>& out) {
    const ValidateCtx c{&tool_name, &param_path, &out};

    // type：类型不匹配时其余约束无意义，直接返回
    if (schema.contains("type") && schema["type"].is_string()) {
        const std::string expected = schema["type"].get<std::string>();
        if (!type_matches(expected, value)) {
            add_error(c, expected, json_type_name(value), "参数类型不匹配");
            return;
        }
    }

    validate_enum(schema, value, c);
    validate_range(schema, value, c);
    validate_length(schema, value, c);
    validate_pattern(schema, value, c);
    validate_items(schema, value, tool_name, param_path, depth, out);

    // 嵌套 object（受限递归）
    if (value.is_object() && schema.contains("properties") &&
        schema["properties"].is_object() && depth > 0) {
        validate_properties(schema, value, tool_name, param_path, depth - 1, out);
    }
}

/// @brief 校验 object 的 properties / required
void validate_properties(const nlohmann::json& schema,
                         const nlohmann::json& value,
                         const std::string& tool_name,
                         const std::string& param_path,
                         int depth,
                         std::vector<SchemaError>& out) {
    const ValidateCtx c{&tool_name, &param_path, &out};

    // required
    if (schema.contains("required") && schema["required"].is_array()) {
        for (const auto& req : schema["required"]) {
            if (!req.is_string()) continue;
            if (value.contains(req.get<std::string>())) continue;
            const std::string p = param_path.empty()
                                      ? "/" + req.get<std::string>()
                                      : param_path + "/" + req.get<std::string>();
            const ValidateCtx rc{&tool_name, &p, &out};
            add_error(rc, req.get<std::string>(), "missing", "缺少必填字段", /*is_missing=*/true);
        }
    }

    // properties
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            const std::string key = it.key();
            if (!value.contains(key)) continue;  // 非必填且未提供，跳过
            const std::string cpath = param_path.empty() ? "/" + key : param_path + "/" + key;
            validate_value(it.value(), value.at(key), tool_name, cpath, depth, out);
        }
    }

    // additionalProperties: false 仅告警不拒绝（定稿决策：弱模型兼容优先）
    // 首版不做字段拦截，保留扩展点。
}

} // namespace

std::string SchemaError::to_string() const {
    std::ostringstream oss;
    oss << "[" << tool << "] param " << param
        << ": expected " << expected << ", actual " << actual;
    if (!hint.empty()) oss << " (" << hint << ")";
    return oss.str();
}

std::string SchemaResult::to_string() const {
    if (ok) return "";
    std::ostringstream oss;
    for (size_t i = 0; i < errors.size(); ++i) {
        if (i) oss << "\n";
        oss << errors[i].to_string();
    }
    return oss.str();
}

SchemaResult validate_schema(const nlohmann::json& schema,
                             const nlohmann::json& input,
                             const std::string& tool_name) {
    SchemaResult result;

    // schema 为空 / 非 object 时不校验（无约束声明）
    if (schema.is_null() || schema.empty() || !schema.is_object()) {
        return result;
    }

    // 顶层 type 检查（工具 schema 通常为 "object"）
    if (schema.contains("type") && schema["type"].is_string() &&
        schema["type"].get<std::string>() == "object") {
        if (!input.is_object()) {
            SchemaError e;
            e.tool = tool_name;
            e.param = "";
            e.expected = "object";
            e.actual = json_type_name(input);
            e.hint = "工具参数应为 JSON 对象";
            result.errors.push_back(std::move(e));
            result.ok = false;
            return result;
        }
    }

    // 校验 properties / required（递归深度上限 3，超出跳过深层校验）
    std::vector<SchemaError> errs;
    validate_properties(schema, input, tool_name, "", /*depth=*/3, errs);

    if (!errs.empty()) {
        result.ok = false;
        result.errors = std::move(errs);
    }
    return result;
}

} // namespace agent::util
