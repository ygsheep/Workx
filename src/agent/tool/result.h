/**
 * @file result.h
 * @brief ToolResult — 工具结果类型（V2：成功载荷）
 * @details V2-4 迁移：ToolResult 仅表示成功载荷（type/text/data），
 *          错误状态由 ResultV2<ToolResult> 承载。
 *          移除了 is_error 字段、Type::Error 枚举值、error() 工厂方法。
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "agent/tool/encoding.h"

namespace agent::tool {

/// @brief 工具结果类型（V2：成功载荷）
///
/// 仅封装工具成功执行后的返回值：
/// - 文本输出或结构化数据
/// - 错误状态由上层 ResultV2<ToolResult> 承载
struct ToolResult {
    /// @brief 结果内容类型
    enum class Type {
        Text,  ///< 文本输出
        Json,  ///< 结构化数据
    };

    Type type{Type::Text};  ///< 内容类型
    std::string text;       ///< 文本内容
    nlohmann::json data;    ///< 结构化数据（可选）

    /// @brief 创建文本结果
    /// @param text 文本内容
    /// @return 文本结果
    static ToolResult ok(std::string text) {
        return {.type = Type::Text, .text = std::move(text), .data = {}};
    }

    /// @brief 创建结构化数据结果
    /// @param data JSON 数据
    /// @return 结构化结果
    static ToolResult ok(nlohmann::json data) {
        return {.type = Type::Json, .text = {}, .data = std::move(data)};
    }

    /// @brief 格式化为 LLM 可读的文本
    /// @return 格式化后的文本
    /// @note 文本与 JSON 数据在序列化前均做 UTF-8 清洗，避免工具输出
    ///       含非 UTF-8 字节（如 GBK 子进程 stdout）时在 nlohmann::json
    ///       dump() 阶段抛出 type_error.316。
    std::string to_string() const {
        if (type == Type::Json) {
            return sanitize_json_strings(data).dump(2);
        }
        return sanitize_utf8(text);
    }
};

}  // namespace agent::tool
