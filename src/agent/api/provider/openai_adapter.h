/**
 * @file openai_adapter.h
 * @brief OpenAI 协议适配器
 * @details 实现 OpenAI chat/completions API 格式
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include "agent/api/provider/i_provider_adapter.h"

namespace agent {

/// @brief OpenAI 协议适配器
/// @details 支持 OpenAI 官方 API 及兼容 API（DeepSeek、Groq、Together 等）
class OpenAIAdapter : public IProviderAdapter {
   public:
    ProviderType type() const override { return ProviderType::OpenAI; }

    std::string build_url(const std::string& base_url) const override;

    std::vector<std::pair<std::string, std::string>> build_headers(
        const std::string& api_key) const override;

    std::string build_request_body(const CompletionRequest& request,
                                   const std::string& model_name) const override;

    bool parse_sse_event(const std::string& event_type, const std::string& data,
                         StreamChunk& out) const override;

    /// @brief OpenAI 支持 /v1/models 端点
    ModelEndpointResult get_models_endpoint() const override;

    /// @brief DS_CACHE P2：设置是否把 reasoning_content 往返发送给模型
    /// @details DeepSeek-reasoner 支持 reasoning_content 字段往返，CoT 进前缀可提升
    ///          多轮对话缓存命中率。默认 false（不发送，兼容 Gemma 等非标准模型）。
    ///          仅对 DeepSeek-reasoner 等 thinking 模型开启。
    /// @note 必须在 build_request_body 调用前设置
    void set_send_reasoning_content(bool enabled) { m_send_reasoning_content = enabled; }
    bool send_reasoning_content() const { return m_send_reasoning_content; }

   private:
    bool m_send_reasoning_content = false;  ///< DS_CACHE P2：是否往返 reasoning_content
};

}  // namespace agent
