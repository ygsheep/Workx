/**
 * @file provider_preset.h
 * @brief Provider 预设注册表
 * @details 内置提供商预设（OpenAI、Anthropic、DeepSeek 等），
 *          用户只需选择名称 + 输入 API Key
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include "agent/model/provider_type.h"

namespace agent {

/// @brief Provider 预设
/// @details 一个预设包含名称、协议类型、默认 URL 和默认模型
struct ProviderPreset {
    std::string name;           ///< 内部名称，如 "deepseek"
    std::string display_name;   ///< 显示名，如 "DeepSeek"
    ProviderType type;          ///< 协议类型
    std::string default_url;    ///< 默认 API 基础 URL
    std::string default_model;  ///< 默认模型名
    std::string api_path;       ///< API 路径，如 "/v1/chat/completions"
    int timeout_ms = 0;         ///< 预设超时（毫秒），0 表示使用全局默认
    int retry_delay_ms = 0;     ///< 预设重试延迟（毫秒），0 表示使用全局默认
    int32_t default_context_length = 0;  ///< 默认模型上下文窗口（token），0 表示未知
};

/// @brief 构建完整的 Preset URL（base_url + api_path，去重尾部 /）
/// @param preset Preset 指针
/// @return 完整 API URL（如 "https://api.deepseek.com/v1/chat/completions"）；
///         std::nullopt 表示 preset 为空或 default_url 未设置，调用方需手动指定 URL。
std::optional<std::string> build_preset_url(const ProviderPreset* preset);

/// @brief 查找预设
/// @param name 预设名称（小写，如 "openai", "anthropic", "deepseek"）
/// @return 找到返回指针，否则 nullptr
const ProviderPreset* find_preset(std::string_view name);

/// @brief 获取所有预设名列表
/// @return 预设名称的 vector（如 ["openai", "anthropic", "deepseek", ...]）
/// @note 返回 string_view 指向静态 Preset 内部 std::string，调用方需注意：
///       仅在未重新加载预设表时安全；预设表当前为静态常量，可视为永久有效。
std::vector<std::string_view> list_preset_names();

}  // namespace agent
