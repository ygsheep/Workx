/**
 * @file types.h
 * @brief 命令相关类型定义
 * @details Availability, LoadSource, CommandContext, CommandResult, PromptBlock 等类型
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace agent::command {

/// 命令可用性 — 对应 CommandAvailability
enum class Availability {
    Universal,  ///< 无限制，默认
    ClaudeAI,   ///< 仅 claude.ai 订阅用户
    Console,    ///< 仅 Console API key 用户
};

/// 命令加载来源 — 对应 loadedFrom
enum class LoadSource {
    Builtin,
    Skills,
    Plugin,
    Bundled,
    MCP,
    Deprecated,
};

/// 命令执行上下文
/// 对应精简版 LocalJSXCommandContext
struct CommandContext {
    std::string cwd;         ///< 工作目录
    std::string model;       ///< 当前模型名称
    std::string session_id;  ///< 会话 ID
    nlohmann::json options;  ///< 额外选项
};

/// 命令结果 — 对应 LocalCommandResult
struct CommandResult {
    enum class Type {
        Text,     ///< 文本输出
        Compact,  ///< 压缩结果
        Skip,     ///< 跳过消息
    };

    Type type{Type::Text};
    std::string text;             ///< 文本输出内容
    nlohmann::json compact_data;  ///< 压缩结果数据（可选）
    bool is_error{false};         ///< 是否错误

    static CommandResult ok(std::string text) {
        return {.type = Type::Text, .text = std::move(text), .compact_data = {}, .is_error = false};
    }

    static CommandResult error(std::string msg) {
        return {.type = Type::Text, .text = std::move(msg), .compact_data = {}, .is_error = true};
    }
};

/// 提示词内容块类型 — 对应 ContentBlockParam 的 type 字段
enum class PromptBlockType {
    Text,        ///< "text"
    Image,       ///< "image"
    ToolResult,  ///< "tool_result"
};

/// @brief 字符串 → PromptBlockType 映射；未识别值返回 PromptBlockType::Text
inline PromptBlockType prompt_block_type_from_string(std::string_view s) {
    if (s == "image") return PromptBlockType::Image;
    if (s == "tool_result") return PromptBlockType::ToolResult;
    return PromptBlockType::Text;
}

/// @brief PromptBlockType → 字符串（与 Anthropic API 字面量一致）
inline std::string_view to_string_view(PromptBlockType t) {
    switch (t) {
        case PromptBlockType::Text:
            return "text";
        case PromptBlockType::Image:
            return "image";
        case PromptBlockType::ToolResult:
            return "tool_result";
    }
    return "text";
}

/// 提示词内容块 — 对应 ContentBlockParam
struct PromptBlock {
    PromptBlockType type{PromptBlockType::Text};  ///< 内容块类型
    std::string text;                             ///< 文本内容
    nlohmann::json image;                         ///< 图片内容（可选）
};

}  // namespace agent::command
