/**
 * @file i_provider_adapter.h
 * @brief Provider 协议适配器接口
 * @details 封装不同 API 提供商在 URL、认证、请求体、SSE 解析上的差异
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <utility>
#include "agent/model/provider_type.h"
#include "agent/api/chat_types.h"

namespace agent {

/// @brief Provider 协议适配器接口
/// @details RemoteBackend 通过此接口屏蔽 OpenAI/Anthropic 等 API 差异
class IProviderAdapter {
   public:
    virtual ~IProviderAdapter() = default;

    /// @brief 协议类型
    virtual ProviderType type() const = 0;

    /// @brief 构建完整 API URL
    /// @param base_url 基础 URL（不含路径）
    virtual std::string build_url(const std::string& base_url) const = 0;

    /// @brief 构建 HTTP 请求头列表
    /// @param api_key API 密钥
    /// @return header 键值对列表
    virtual std::vector<std::pair<std::string, std::string>> build_headers(
        const std::string& api_key) const = 0;

    /// @brief 构建请求体 JSON
    /// @param request 推理请求
    /// @param model_name 模型名称
    virtual std::string build_request_body(const CompletionRequest& request,
                                           const std::string& model_name) const = 0;

    /// @brief 解析 SSE 事件
    /// @param event_type SSE event 类型（OpenAI 通常为空）
    /// @param data SSE data 内容（JSON 字符串）
    /// @param out 输出的 StreamChunk
    /// @return true 如果解析出了有效 chunk
    virtual bool parse_sse_event(const std::string& event_type, const std::string& data,
                                 StreamChunk& out) const = 0;

    /// @brief 是否支持 list_models HTTP 端点
    /// @details Anthropic 无公开 list models 端点，返回 {false, ""}
    ///          OpenAI 等兼容 API 返回 {true, "/v1/models"}
    struct ModelEndpointResult {
        bool supported = false;  ///< 是否支持 list_models 端点
        std::string url_suffix;  ///< URL 后缀（如 "/v1/models"）
    };
    virtual ModelEndpointResult get_models_endpoint() const {
        return {false, ""};  // 默认不支持
    }

    /// @brief 内置模型列表（当 get_models_endpoint().supported=false 时使用）
    /// @details Anthropic 等无 list models 端点的 provider 覆盖此方法返回内置列表。
    ///          base_url 用于区分同协议下不同 provider（如 DeepSeek 的 Anthropic 兼容端点）。
    virtual std::vector<ModelInfo> get_builtin_models(const std::string& base_url = "") const {
        (void)base_url;  // 默认实现忽略
        return {};       // 默认空
    }
};

}  // namespace agent
