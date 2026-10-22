/**
 * @file mcp_transport.cpp
 * @brief MCP 传输层实现（stdio）
 * @details StdioMcpTransport 通过持久子进程收发 JSON-RPC 消息：
 *          - send_request：写 JSON 行 → 循环读行直到匹配 id 的响应（带总超时）
 *          - 忽略非 JSON 行（server 日志）与不匹配 id 的消息（notification/其他响应）
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_transport.h"

#include <chrono>
#include <sstream>

namespace agent::mcp {

StdioMcpTransport::StdioMcpTransport(McpServerConfig cfg) : m_cfg(std::move(cfg)) {}

StdioMcpTransport::~StdioMcpTransport() { stop(); }

ResultV2<void> StdioMcpTransport::start() {
    if (m_started) return ResultV2<void>::ok();
    auto result = m_proc.start(m_cfg.command, m_cfg.args, m_cfg.env);
    if (result.is_err()) return result;
    m_started = true;
    return ResultV2<void>::ok();
}

void StdioMcpTransport::stop() {
    if (!m_started) return;
    m_proc.stop();
    m_started = false;
}

ResultV2<nlohmann::json> StdioMcpTransport::send_request(const nlohmann::json& msg,
                                                         int timeout_ms) {
    if (!msg.contains("id")) {
        return ResultV2<nlohmann::json>::err(Error::Code::InvalidInput, "JSON-RPC 请求缺少 id",
                                             "StdioMcpTransport::send_request");
    }
    const auto id = msg.at("id");

    auto write = m_proc.write_line(msg.dump());
    if (write.is_err()) return write.error();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    // 忽略非 JSON 行上限，防止 server 异常输出刷爆队列
    int ignored_lines = 0;
    constexpr int kMaxIgnoredLines = 1000;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return ResultV2<nlohmann::json>::err(
                Error::Code::NetworkTimeout, "MCP 请求超时 (" + std::to_string(timeout_ms) + "ms)",
                "method=" + msg.value("method", ""));
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

        auto line = m_proc.read_line(static_cast<int>(remaining));
        if (line.is_err()) return line.error();

        nlohmann::json resp;
        try {
            resp = nlohmann::json::parse(line.value());
        } catch (const nlohmann::json::exception&) {
            // 非 JSON 行（server 日志）：忽略
            if (++ignored_lines > kMaxIgnoredLines) {
                return ResultV2<nlohmann::json>::err(Error::Code::StreamError,
                                                     "MCP server 输出过多非 JSON 数据",
                                                     "StdioMcpTransport::send_request");
            }
            continue;
        }

        // 只接受匹配 id 的响应；notification（无 id）与不匹配 id 的消息忽略
        if (resp.is_object() && resp.contains("id") && resp.at("id") == id) {
            return ResultV2<nlohmann::json>::ok(std::move(resp));
        }
    }
}

ResultV2<void> StdioMcpTransport::send_notification(const nlohmann::json& msg) {
    return m_proc.write_line(msg.dump());
}

// ============================================================
// HttpMcpTransport（Streamable HTTP，M3）
// ============================================================

HttpMcpTransport::HttpMcpTransport(McpServerConfig cfg) : m_cfg(std::move(cfg)) {
    // SSRF 防护：URL 来自用户配置（可能来自克隆项目的 .mcp.json），默认拦截
    // 内网/回环地址；本地 MCP server 通过 allowPrivate=true 显式放行。
    m_http.set_block_private_ips(!m_cfg.allow_private);
}

HttpMcpTransport::~HttpMcpTransport() { stop(); }

ResultV2<void> HttpMcpTransport::start() {
    if (m_started) return ResultV2<void>::ok();
    const auto& url = m_cfg.url;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        return ResultV2<void>::err(Error::Code::InvalidInput,
                                   "MCP HTTP server URL 必须是 http/https: " + url,
                                   "HttpMcpTransport::start");
    }
    m_started = true;
    return ResultV2<void>::ok();
}

void HttpMcpTransport::stop() {
    m_started = false;
    m_session_id.clear();
}

std::vector<std::pair<std::string, std::string>> HttpMcpTransport::build_headers() const {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    if (!m_session_id.empty()) {
        headers.emplace_back("Mcp-Session-Id", m_session_id);
    }
    for (const auto& [k, v] : m_cfg.headers) {
        headers.emplace_back(k, v);
    }
    return headers;
}

ResultV2<nlohmann::json> HttpMcpTransport::send_request(const nlohmann::json& msg, int timeout_ms) {
    if (!m_started) {
        return ResultV2<nlohmann::json>::err(Error::Code::NetworkDisconnected,
                                             "MCP HTTP 传输未启动",
                                             "HttpMcpTransport::send_request");
    }
    if (!msg.contains("id")) {
        return ResultV2<nlohmann::json>::err(Error::Code::InvalidInput, "JSON-RPC 请求缺少 id",
                                             "HttpMcpTransport::send_request");
    }

    auto resp = m_http.post(m_cfg.url, build_headers(), msg.dump(), timeout_ms);
    if (resp.is_err()) return resp.error();

    const auto& r = resp.value();
    if (!r.is_success()) {
        return ResultV2<nlohmann::json>::err(
            Error::Code::NetworkDisconnected,
            "MCP HTTP 请求失败: HTTP " + std::to_string(r.status_code), "url=" + m_cfg.url);
    }

    // 捕获 1.x 会话头（Mcp-Session-Id），后续请求回传
    // P2-5：校验格式，拒绝含 CRLF/控制字符的会话 ID（防 header 注入）
    for (const auto& [k, v] : r.headers) {
        if (k == "mcp-session-id") {
            bool safe = !v.empty();
            for (unsigned char c : v) {
                if (c < 0x20 || c == 0x7F) {
                    safe = false;
                    break;
                }
            }
            m_session_id = safe ? v : std::string();
        }
    }

    std::string content_type;
    for (const auto& [k, v] : r.headers) {
        if (k == "content-type") {
            content_type = v;
            break;
        }
    }

    auto parsed = parse_response_body(r.body, content_type);
    // 校验返回的响应 id 与请求一致（SSE 流可能含多条消息）
    if (parsed.is_object() && parsed.contains("id") && parsed.at("id") != msg.at("id")) {
        return ResultV2<nlohmann::json>::err(Error::Code::StreamError, "MCP HTTP 响应 id 不匹配",
                                             "HttpMcpTransport::send_request");
    }
    return ResultV2<nlohmann::json>::ok(std::move(parsed));
}

ResultV2<void> HttpMcpTransport::send_notification(const nlohmann::json& msg) {
    if (!m_started) {
        return ResultV2<void>::err(Error::Code::NetworkDisconnected, "MCP HTTP 传输未启动",
                                   "HttpMcpTransport::send_notification");
    }
    auto resp = m_http.post(m_cfg.url, build_headers(), msg.dump(), 15000);
    if (resp.is_err()) return resp.error();
    if (!resp.value().is_success()) {
        return ResultV2<void>::err(
            Error::Code::NetworkDisconnected,
            "MCP HTTP 通知失败: HTTP " + std::to_string(resp.value().status_code),
            "url=" + m_cfg.url);
    }
    return ResultV2<void>::ok();
}

nlohmann::json HttpMcpTransport::parse_response_body(const std::string& body,
                                                     const std::string& content_type) {
    // SSE 响应：逐事件解析，返回第一个含 id 的 JSON-RPC 消息
    if (content_type.find("text/event-stream") != std::string::npos) {
        for (const auto& ev : parse_sse(body)) {
            if (ev.is_object() && ev.contains("id")) return ev;
        }
        return nlohmann::json::object();
    }
    // JSON 响应
    try {
        return nlohmann::json::parse(body);
    } catch (const nlohmann::json::exception&) {
        return nlohmann::json::object();
    }
}

std::vector<nlohmann::json> HttpMcpTransport::parse_sse(const std::string& body) {
    std::vector<nlohmann::json> events;
    std::string data;
    auto flush = [&]() {
        if (!data.empty()) {
            try {
                events.push_back(nlohmann::json::parse(data));
            } catch (const nlohmann::json::exception&) {
                // 忽略非 JSON 的 data 块
            }
            data.clear();
        }
    };

    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            flush();  // 空行分隔事件
        } else if (line.rfind("data:", 0) == 0) {
            std::string payload = line.substr(5);
            if (!payload.empty() && payload[0] == ' ') payload.erase(0, 1);
            if (!data.empty()) data += "\n";
            data += payload;
        }
        // 忽略 event:/id:/retry: 等其他字段
    }
    flush();
    return events;
}

std::unique_ptr<McpTransport> create_transport(const McpServerConfig& cfg) {
    if (cfg.is_http()) {
        return std::make_unique<HttpMcpTransport>(cfg);
    }
    return std::make_unique<StdioMcpTransport>(cfg);
}

}  // namespace agent::mcp
