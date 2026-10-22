/**
 * @file provider_type.h
 * @brief Provider 类型枚举
 * @details 定义 API 提供商协议类型，如 OpenAI、Anthropic
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <format>
#include <string>
#include <string_view>
#include <algorithm>
#include <cctype>
#include "core/utils/result.h"

namespace agent {

/// @brief API 提供商协议类型
/// @details 决定使用哪种协议格式（URL、认证、请求体、SSE 解析）
enum class ProviderType {
    OpenAI = 0,    ///< OpenAI 协议格式（含兼容 API：DeepSeek、Groq、Together 等）
    Anthropic = 1  ///< Anthropic Messages API
};

/// @brief 转换为字符串（小写）
constexpr std::string_view to_string(ProviderType type) {
    switch (type) {
        case ProviderType::OpenAI:
            return "openai";
        case ProviderType::Anthropic:
            return "anthropic";
    }
    return "unknown";
}

/// @brief 从字符串解析（大小写不敏感）
/// @details 统一转小写后比较，支持任意大小写组合（如 "OpenAI"、"OPENAI"、"openai"）
inline Result<ProviderType, std::string> provider_type_from_string(std::string_view str) {
    std::string lower{str};
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "openai") {
        return Result<ProviderType, std::string>::ok(ProviderType::OpenAI);
    }
    if (lower == "anthropic") {
        return Result<ProviderType, std::string>::ok(ProviderType::Anthropic);
    }
    return Result<ProviderType, std::string>::err(std::format("Unknown provider type: '{}'", str));
}

}  // namespace agent
