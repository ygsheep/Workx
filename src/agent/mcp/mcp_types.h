/**
 * @file mcp_types.h
 * @brief MCP 协议消息结构（Issue #27）
 * @details Model Context Protocol 客户端所需的协议数据结构：
 *          工具/资源信息、JSON-RPC 错误码、协议版本常量。
 *          兼容 1.x（2025-11-25）与 2.0（2026-07-28）协议。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::mcp {

/// MCP 协议版本常量（2.0 无状态协议 + 1.x 握手协议）
inline constexpr const char* kProtocolVersion2026_07_28 = "2026-07-28";
inline constexpr const char* kProtocolVersion2025_11_25 = "2025-11-25";
inline constexpr const char* kProtocolVersion2025_03_26 = "2025-03-26";
inline constexpr const char* kProtocolVersion2024_11_05 = "2024-11-05";

/// 当前客户端首选协议版本（2.0，协商失败回退 1.x）
inline constexpr const char* kPreferredProtocolVersion = kProtocolVersion2026_07_28;

/// JSON-RPC 2.0 标准错误码
namespace jsonrpc_error {
inline constexpr int ParseError = -32700;
inline constexpr int InvalidRequest = -32600;
inline constexpr int MethodNotFound = -32601;
inline constexpr int InvalidParams = -32602;
inline constexpr int InternalError = -32603;
}  // namespace jsonrpc_error

/// MCP 协议错误码
namespace mcp_error {
/// 协议版本不匹配（1.x initialize 返回该错误，客户端用返回版本重试）
inline constexpr int UnsupportedProtocolVersion = -32602;
}  // namespace mcp_error

/// @brief 2.0 缓存元数据（响应 _meta.ttlMs / _meta.cacheScope）
/// @details ttl_ms > 0 表示客户端可在该时长内缓存响应；cache_scope 取值
///          "session"（会话内共享）或 "shared"（跨会话共享）。
struct McpCacheMeta {
    int64_t ttl_ms = 0;       ///< 缓存有效期（毫秒，0=不缓存）
    std::string cache_scope;  ///< 缓存作用域（"session" / "shared" / 空）
};

/// @brief MCP server 暴露的工具信息（tools/list 响应项）
struct McpToolInfo {
    std::string name;
    std::string description;
    nlohmann::json input_schema;  ///< JSON Schema（2.0 支持 2020-12 全词汇表）
    McpCacheMeta cache;           ///< 2.0 响应缓存元数据（无则 ttl_ms=0）
};

/// @brief MCP server 暴露的资源信息（resources/list 响应项）
struct McpResourceInfo {
    std::string uri;
    std::string name;
    std::string mime_type;
    std::string description;
    McpCacheMeta cache;  ///< 2.0 响应缓存元数据（无则 ttl_ms=0）
};

/// @brief 资源内容（resources/read 响应项）
struct McpResourceContent {
    std::string uri;
    std::string mime_type;
    std::string text;    ///< 文本内容
    std::string blob;    ///< 二进制内容（base64 编码）
    McpCacheMeta cache;  ///< 2.0 响应缓存元数据（无则 ttl_ms=0）
};

/// @brief 资源模板信息（resources/templates/list 响应项，M4）
struct McpResourceTemplateInfo {
    std::string uri_template;  ///< 模板（如 "git:///{owner}/{repo}/blob/{sha}"）
    std::string name;
    std::string description;
    std::string mime_type;
};

/// @brief 提示词参数定义（prompts/list 响应项 arguments[] 项，M4）
struct McpPromptArgument {
    std::string name;
    std::string description;
    bool required = false;
};

/// @brief 提示词信息（prompts/list 响应项，M4）
struct McpPromptInfo {
    std::string name;
    std::string description;
    std::vector<McpPromptArgument> arguments;
};

/// @brief 提示词消息（prompts/get 响应 messages[] 项，M4）
struct McpPromptMessage {
    std::string role;        ///< user / assistant
    nlohmann::json content;  ///< 内容块（text / image / resource）
};

/// @brief 工具调用结果内容块（tools/call 响应 content[] 项）
struct McpCallContent {
    std::string type;  ///< text / image / resource
    std::string text;  ///< type=text 时有效
    std::string mime_type;
    std::string data;  ///< type=image 时有效（base64）
};

/// @brief 工具调用结果（tools/call 响应）
struct McpCallResult {
    std::vector<McpCallContent> content;
    bool is_error = false;
};

}  // namespace agent::mcp
