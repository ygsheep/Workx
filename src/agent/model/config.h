/**
 * @file config.h
 * @brief 模型静态配置表
 * @details 定义各模型的上下文窗口、最大输出 token 等静态能力。
 *          用于上下文管理（autocompact 阈值、UI 百分比显示）。
 *          与 claude-code utils/context.ts 对齐。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>

namespace agent {

/// @brief 模型能力配置
/// @details 静态字段由厂商规范决定；用户不可配置。
struct ModelCapability {
    std::string canonical_name;     ///< 规范名（如 "claude-sonnet-4-5"）
    int32_t context_window = 0;     ///< 上下文窗口 token 数
    int32_t max_output_tokens = 0;  ///< 单次响应最大输出 token 数
    bool supports_tools = false;    ///< 是否支持 function calling
    bool supports_vision = false;   ///< 是否支持图像输入
};

/// @brief 默认上下文窗口（兜底值，未知模型使用）
constexpr int32_t MODEL_CONTEXT_WINDOW_DEFAULT = 200'000;

/// @brief 默认最大输出 token（兜底值）
constexpr int32_t MAX_OUTPUT_TOKENS_DEFAULT = 32'000;

/// @brief 最大输出 token 上限（兜底值）
constexpr int32_t MAX_OUTPUT_TOKENS_UPPER_LIMIT = 64'000;

/// @brief 模糊匹配查找模型能力
/// @details 按子串匹配 canonical_name（不区分大小写）。
///          例如 "claude-sonnet-4-5-20250929" 匹配 "claude-sonnet-4-5"。
///          匹配优先级：先精确，再最长前缀。
/// @param model_name 用户输入的模型名（如 "claude-sonnet-4-5-20250929"）
/// @return 找到返回指针，否则 nullptr
const ModelCapability* find_model_capability(std::string_view model_name);

/// @brief 获取模型的上下文窗口大小
/// @details 优先级：find_model_capability → MODEL_CONTEXT_WINDOW_DEFAULT
int32_t get_context_window_for_model(std::string_view model_name);

/// @brief 获取模型的最大输出 token 数
/// @details 优先级：find_model_capability → MAX_OUTPUT_TOKENS_DEFAULT
int32_t get_max_output_tokens_for_model(std::string_view model_name);

}  // namespace agent
