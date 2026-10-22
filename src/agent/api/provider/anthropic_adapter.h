/**
 * @file anthropic_adapter.h
 * @brief Anthropic 协议适配器
 * @details 实现 Anthropic Messages API 格式
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include "agent/api/provider/i_provider_adapter.h"

namespace agent {

/// @brief Anthropic 协议适配器
/// @details 适配 Anthropic Claude Messages API（/v1/messages）
class AnthropicAdapter : public IProviderAdapter {
   public:
    ProviderType type() const override { return ProviderType::Anthropic; }

    std::string build_url(const std::string& base_url) const override;

    std::vector<std::pair<std::string, std::string>> build_headers(
        const std::string& api_key) const override;

    std::string build_request_body(const CompletionRequest& request,
                                   const std::string& model_name) const override;

    bool parse_sse_event(const std::string& event_type, const std::string& data,
                         StreamChunk& out) const override;

    /// @brief Anthropic 无公开 list_models 端点，返回内置模型列表
    /// @details 根据 base_url 区分：DeepSeek 的 Anthropic 兼容端点返回 DeepSeek 模型，
    ///          原生 Anthropic 端点返回 Claude 模型列表
    std::vector<ModelInfo> get_builtin_models(const std::string& base_url = "") const override;
};

}  // namespace agent
