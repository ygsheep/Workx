/**
 * @file mcp_transport.h
 * @brief MCP 传输层抽象（Issue #27）
 * @details 传输层负责 JSON-RPC 消息的收发：
 *          - StdioMcpTransport：spawn 持久子进程，stdin 写 / stdout 行读
 *          - HttpMcpTransport：Streamable HTTP（M3 实现）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/api/remote/http_client.h"
#include "agent/mcp/mcp_config.h"
#include "agent/mcp/mcp_stdio_process.h"
#include "core/utils/result_v2.h"

namespace agent::mcp {

/// @brief 传输抽象：发送 JSON-RPC 消息并同步等待响应
class McpTransport {
   public:
    virtual ~McpTransport() = default;

    /// @brief 启动传输（spawn 子进程 / 建立连接）
    virtual ResultV2<void> start() = 0;
    /// @brief 停止传输（终止子进程 / 关闭连接，幂等）
    virtual void stop() = 0;

    /// @brief 发送请求（单条 JSON-RPC 消息），同步阻塞等待匹配 id 的响应
    /// @param msg JSON-RPC 请求（含 id）
    /// @param timeout_ms 总超时（含等待响应）
    /// @return ok: 完整 JSON-RPC 响应（含 result 或 error）；err: 传输错误/超时
    virtual ResultV2<nlohmann::json> send_request(const nlohmann::json& msg, int timeout_ms) = 0;

    /// @brief 发送通知（无需响应）
    virtual ResultV2<void> send_notification(const nlohmann::json& msg) = 0;
};

/// @brief stdio 传输：持久子进程 + 双向管道
class StdioMcpTransport : public McpTransport {
   public:
    explicit StdioMcpTransport(McpServerConfig cfg);
    ~StdioMcpTransport() override;

    ResultV2<void> start() override;
    void stop() override;
    ResultV2<nlohmann::json> send_request(const nlohmann::json& msg, int timeout_ms) override;
    ResultV2<void> send_notification(const nlohmann::json& msg) override;

   private:
    McpServerConfig m_cfg;
    McpStdioProcess m_proc;
    bool m_started = false;
};

/// @brief Streamable HTTP 传输：POST JSON-RPC 到 server URL（MCP 2.0 / 1.x）
/// @details
/// - 复用 HttpClient（同步 POST），SSRF 防护默认开启（allow_private=true 时放行内网）
/// - 响应支持 application/json 与 text/event-stream 两种 Content-Type
/// - 1.x 会话：捕获响应头 Mcp-Session-Id 并在后续请求回传
class HttpMcpTransport : public McpTransport {
   public:
    explicit HttpMcpTransport(McpServerConfig cfg);
    ~HttpMcpTransport() override;

    ResultV2<void> start() override;
    void stop() override;
    ResultV2<nlohmann::json> send_request(const nlohmann::json& msg, int timeout_ms) override;
    ResultV2<void> send_notification(const nlohmann::json& msg) override;

   private:
    /// 构造请求头（Content-Type / Accept / 会话头 / 用户配置头）
    std::vector<std::pair<std::string, std::string>> build_headers() const;
    /// 解析响应体（JSON 或 SSE），返回 JSON-RPC 消息
    static nlohmann::json parse_response_body(const std::string& body,
                                              const std::string& content_type);
    /// 解析 SSE 事件流，返回其中所有 JSON 消息
    static std::vector<nlohmann::json> parse_sse(const std::string& body);

    McpServerConfig m_cfg;
    HttpClient m_http;
    std::string m_session_id;  ///< 1.x 会话头（Mcp-Session-Id）
    bool m_started = false;
};

/// @brief 创建传输实例（按配置选择 stdio/http）
std::unique_ptr<McpTransport> create_transport(const McpServerConfig& cfg);

}  // namespace agent::mcp
