#pragma once

/// @file json_schema.h
/// @brief JSON Schema 轻量验证（Issue #80）
///
/// 对工具参数进行 JSON Schema 验证，实现工具 schema 声明中实际用到的关键字子集：
/// - type（string / number / integer / boolean / array / object / null）
/// - properties（浅层遍历，嵌套 object 递归受限）
/// - required
/// - enum
/// - minimum / maximum（number）
/// - minLength / maxLength（string）
/// - pattern（string，std::regex）
/// - items（array 元素浅层校验）
///
/// 不依赖外部库，轻量实现（对齐 issue 原始设计意图）。
/// 校验为纯函数、无状态、无副作用，便于单元测试。
/// additionalProperties: false 仅告警不拒绝（宽松，弱模型兼容优先）。

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent::util {

/// @brief 单条 schema 校验错误（结构化，供 LLM 回灌自纠）
struct SchemaError {
    std::string tool;       ///< 工具名
    std::string param;      ///< 出错参数路径（JSON Pointer 风格，如 "/content"）
    std::string expected;   ///< 期望值（如 "string"、"one of [a,b,c]"）
    std::string actual;     ///< 实际值描述（如 "number"、具体值）
    std::string hint;       ///< 可读提示

    /// @brief 是否为"缺必填"类错误（用于映射 MissingArgument 错误码）
    bool is_missing = false;

    /// @brief 序列化为 LLM 可读单行文本
    std::string to_string() const;
};

/// @brief 校验结果：errors 为空表示通过
struct SchemaResult {
    bool ok = true;
    std::vector<SchemaError> errors;

    /// @brief 汇总所有错误为一段文本
    std::string to_string() const;
};

/// @brief 对工具输入做 schema 校验（纯函数）
///
/// @param schema 工具声明的 input_schema（通常为 {"type":"object", ...}）
/// @param input  工具收到的实际输入参数
/// @param tool_name 工具名（填入错误上下文字段）
/// @return 校验结果；schema 为空 / 非 object 时不校验（返回 ok）
SchemaResult validate_schema(const nlohmann::json& schema,
                             const nlohmann::json& input,
                             const std::string& tool_name);

} // namespace agent::util
