/**
 * @file mcp_client.cpp
 * @brief MCP client 实现
 * @details
 * 协议协商（兼容 1.x 与 2.0）：
 *   ① server/discover（2.0 可选 RPC）→ 成功 → 2.0 无状态模式
 *   ② 失败（MethodNotFound/超时）→ 回退 1.x initialize 握手
 *      - UnsupportedProtocolVersionError → 用更旧版本重试
 *
 * 2.0 无状态模式：每请求 params._meta 携带 io.modelcontextprotocol/protocolVersion。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_client.h"

namespace agent::mcp {

namespace {

/// 从 JSON-RPC error 对象构造 Error
Error error_from_rpc(const nlohmann::json& err, const std::string& method) {
    const int code = err.value("code", 0);
    const std::string message = err.value("message", "unknown error");
    Error::Code ec = Error::Code::ToolExecutionFailed;
    if (code == jsonrpc_error::MethodNotFound) {
        ec = Error::Code::ResourceNotFound;
    } else if (code == jsonrpc_error::InvalidParams) {
        ec = Error::Code::InvalidInput;
    }
    return Error{ec, "MCP 错误: " + message, "method=" + method + "; code=" + std::to_string(code)};
}

/// 判断错误是否为协议版本不匹配（UnsupportedProtocolVersionError）
bool is_unsupported_protocol_version(const nlohmann::json& err) {
    if (!err.is_object()) return false;
    const int code = err.value("code", 0);
    if (code == mcp_error::UnsupportedProtocolVersion) return true;
    const std::string message = err.value("message", "");
    return message.find("protocol version") != std::string::npos ||
           message.find("protocolVersion") != std::string::npos;
}

/// @brief 是否受支持的 MCP 协议版本（P2-8：拒绝未知/降级版本）
bool is_supported_protocol_version(const std::string& v) {
    return v == kProtocolVersion2026_07_28 || v == kProtocolVersion2025_11_25 ||
           v == kProtocolVersion2025_03_26 || v == kProtocolVersion2024_11_05;
}

}  // anonymous namespace

McpClient::McpClient() = default;

McpClient::~McpClient() { disconnect(); }

ResultV2<void> McpClient::connect(const McpServerConfig& cfg, int timeout_ms) {
    m_name = cfg.name;
    m_transport = create_transport(cfg);

    auto start = m_transport->start();
    if (start.is_err()) return start;

    // ① server/discover（2.0 可选 RPC）
    // 注意：transport 层成功（is_ok）不代表 RPC 成功——JSON-RPC error 响应
    // （如 MethodNotFound）同样以 ok 返回，必须排除含 error 字段的响应，
    // 否则 1.x server 会被误判为 2.0 无状态模式。
    auto discover = raw_request("server/discover", nlohmann::json::object(), timeout_ms);
    if (discover.is_ok() && !discover.value().contains("error")) {
        const auto& result = discover.value().value("result", nlohmann::json::object());
        const std::string ver = result.value("protocolVersion", kProtocolVersion2026_07_28);
        if (!is_supported_protocol_version(ver)) {
            m_transport->stop();
            m_transport.reset();
            return ResultV2<void>::err(Error::Code::NetworkDisconnected,
                                       "MCP server 返回不支持的协议版本: " + ver,
                                       "McpClient::connect");
        }
        m_protocol_version = ver;
        m_stateless = true;
        m_connected = true;
        return ResultV2<void>::ok();
    }

    // ② 回退 1.x initialize 握手（尝试新旧版本）
    const char* versions[] = {kProtocolVersion2025_11_25, kProtocolVersion2025_03_26};
    for (const char* version : versions) {
        auto init = do_initialize(version, timeout_ms);
        if (init.is_ok()) {
            const std::string ver = init.value().value("protocolVersion", version);
            if (!is_supported_protocol_version(ver)) {
                m_transport->stop();
                m_transport.reset();
                return ResultV2<void>::err(Error::Code::NetworkDisconnected,
                                           "MCP server 返回不支持的协议版本: " + ver,
                                           "McpClient::connect");
            }
            m_protocol_version = ver;
            m_stateless = false;
            m_connected = true;
            (void)notify("notifications/initialized", nlohmann::json::object());
            return ResultV2<void>::ok();
        }
        // 仅协议版本不匹配时尝试更旧版本
        if (!init.error().context.empty() &&
            init.error().context.find("unsupported_protocol") == std::string::npos) {
            break;
        }
    }

    m_transport->stop();
    m_transport.reset();
    return ResultV2<void>::err(Error::Code::NetworkDisconnected, "MCP 协议协商失败: " + m_name,
                               "McpClient::connect");
}

void McpClient::disconnect() {
    if (m_transport) {
        m_transport->stop();
        m_transport.reset();
    }
    m_connected = false;
}

bool McpClient::is_connected() const { return m_connected; }

const std::string& McpClient::name() const { return m_name; }

const std::string& McpClient::protocol_version() const { return m_protocol_version; }

ResultV2<std::vector<McpToolInfo>> McpClient::list_tools() {
    if (!m_connected) {
        return ResultV2<std::vector<McpToolInfo>>::err(Error::Code::NetworkDisconnected,
                                                       "MCP server 未连接: " + m_name,
                                                       "McpClient::list_tools");
    }
    auto result = request_cached("tools/list", nlohmann::json::object(), 15000);
    if (result.is_err()) return result.error();

    std::vector<McpToolInfo> tools;
    const auto& arr = result.value().value("tools", nlohmann::json::array());
    for (const auto& t : arr) {
        if (!t.is_object() || !t.contains("name")) continue;
        McpToolInfo info;
        info.name = t.at("name").get<std::string>();
        info.description = t.value("description", "");
        info.input_schema = t.value("inputSchema", nlohmann::json::object());
        info.cache = extract_cache_meta(t);
        tools.push_back(std::move(info));
    }
    return ResultV2<std::vector<McpToolInfo>>::ok(std::move(tools));
}

ResultV2<McpCallResult> McpClient::call_tool(const std::string& tool_name,
                                             const nlohmann::json& args) {
    if (!m_connected) {
        return ResultV2<McpCallResult>::err(Error::Code::NetworkDisconnected,
                                            "MCP server 未连接: " + m_name, "McpClient::call_tool");
    }
    nlohmann::json params = {{"name", tool_name},
                             {"arguments", args.is_object() ? args : nlohmann::json::object()}};
    auto result = request("tools/call", params, 30000);
    if (result.is_err()) return result.error();

    McpCallResult call;
    call.is_error = result.value().value("isError", false);
    const auto& content = result.value().value("content", nlohmann::json::array());
    for (const auto& c : content) {
        if (!c.is_object()) continue;
        McpCallContent item;
        item.type = c.value("type", "text");
        item.text = c.value("text", "");
        item.mime_type = c.value("mimeType", "");
        item.data = c.value("data", "");
        call.content.push_back(std::move(item));
    }
    return ResultV2<McpCallResult>::ok(std::move(call));
}

ResultV2<std::vector<McpResourceInfo>> McpClient::list_resources() {
    if (!m_connected) {
        return ResultV2<std::vector<McpResourceInfo>>::err(Error::Code::NetworkDisconnected,
                                                           "MCP server 未连接: " + m_name,
                                                           "McpClient::list_resources");
    }
    auto result = request_cached("resources/list", nlohmann::json::object(), 15000);
    if (result.is_err()) return result.error();

    std::vector<McpResourceInfo> resources;
    const auto& arr = result.value().value("resources", nlohmann::json::array());
    for (const auto& r : arr) {
        if (!r.is_object() || !r.contains("uri")) continue;
        McpResourceInfo info;
        info.uri = r.at("uri").get<std::string>();
        info.name = r.value("name", "");
        info.mime_type = r.value("mimeType", "");
        info.description = r.value("description", "");
        info.cache = extract_cache_meta(r);
        resources.push_back(std::move(info));
    }
    return ResultV2<std::vector<McpResourceInfo>>::ok(std::move(resources));
}

ResultV2<std::vector<McpResourceContent>> McpClient::read_resource(const std::string& uri) {
    if (!m_connected) {
        return ResultV2<std::vector<McpResourceContent>>::err(Error::Code::NetworkDisconnected,
                                                              "MCP server 未连接: " + m_name,
                                                              "McpClient::read_resource");
    }
    nlohmann::json params = {{"uri", uri}};
    auto result = request_cached("resources/read", params, 15000);
    if (result.is_err()) return result.error();

    std::vector<McpResourceContent> contents;
    const auto& arr = result.value().value("contents", nlohmann::json::array());
    for (const auto& c : arr) {
        if (!c.is_object() || !c.contains("uri")) continue;
        McpResourceContent item;
        item.uri = c.at("uri").get<std::string>();
        item.mime_type = c.value("mimeType", "");
        item.text = c.value("text", "");
        item.blob = c.value("blob", "");
        item.cache = extract_cache_meta(c);
        contents.push_back(std::move(item));
    }
    return ResultV2<std::vector<McpResourceContent>>::ok(std::move(contents));
}

// ============================================================
// 资源模板（M4）
// ============================================================

ResultV2<std::vector<McpResourceTemplateInfo>> McpClient::list_resource_templates() {
    if (!m_connected) {
        return ResultV2<std::vector<McpResourceTemplateInfo>>::err(
            Error::Code::NetworkDisconnected, "MCP server 未连接: " + m_name,
            "McpClient::list_resource_templates");
    }
    auto result = request_cached("resources/templates/list", nlohmann::json::object(), 15000);
    if (result.is_err()) return result.error();

    std::vector<McpResourceTemplateInfo> templates;
    const auto& arr = result.value().value("resourceTemplates", nlohmann::json::array());
    for (const auto& t : arr) {
        if (!t.is_object() || !t.contains("uriTemplate")) continue;
        McpResourceTemplateInfo info;
        info.uri_template = t.at("uriTemplate").get<std::string>();
        info.name = t.value("name", "");
        info.description = t.value("description", "");
        info.mime_type = t.value("mimeType", "");
        templates.push_back(std::move(info));
    }
    return ResultV2<std::vector<McpResourceTemplateInfo>>::ok(std::move(templates));
}

// ============================================================
// 提示词（M4）
// ============================================================

ResultV2<std::vector<McpPromptInfo>> McpClient::list_prompts() {
    if (!m_connected) {
        return ResultV2<std::vector<McpPromptInfo>>::err(Error::Code::NetworkDisconnected,
                                                         "MCP server 未连接: " + m_name,
                                                         "McpClient::list_prompts");
    }
    auto result = request_cached("prompts/list", nlohmann::json::object(), 15000);
    if (result.is_err()) return result.error();

    std::vector<McpPromptInfo> prompts;
    const auto& arr = result.value().value("prompts", nlohmann::json::array());
    for (const auto& p : arr) {
        if (!p.is_object() || !p.contains("name")) continue;
        McpPromptInfo info;
        info.name = p.at("name").get<std::string>();
        info.description = p.value("description", "");
        const auto& args = p.value("arguments", nlohmann::json::array());
        for (const auto& a : args) {
            if (!a.is_object() || !a.contains("name")) continue;
            McpPromptArgument arg;
            arg.name = a.at("name").get<std::string>();
            arg.description = a.value("description", "");
            arg.required = a.value("required", false);
            info.arguments.push_back(std::move(arg));
        }
        prompts.push_back(std::move(info));
    }
    return ResultV2<std::vector<McpPromptInfo>>::ok(std::move(prompts));
}

ResultV2<std::vector<McpPromptMessage>> McpClient::get_prompt(const std::string& name,
                                                              const nlohmann::json& arguments) {
    if (!m_connected) {
        return ResultV2<std::vector<McpPromptMessage>>::err(Error::Code::NetworkDisconnected,
                                                            "MCP server 未连接: " + m_name,
                                                            "McpClient::get_prompt");
    }
    nlohmann::json params = {{"name", name}};
    if (arguments.is_object() && !arguments.empty()) {
        params["arguments"] = arguments;
    }
    auto result = request("prompts/get", params, 15000);
    if (result.is_err()) return result.error();

    std::vector<McpPromptMessage> messages;
    const auto& arr = result.value().value("messages", nlohmann::json::array());
    for (const auto& m : arr) {
        if (!m.is_object()) continue;
        McpPromptMessage msg;
        msg.role = m.value("role", "user");
        msg.content = m.value("content", nlohmann::json::object());
        messages.push_back(std::move(msg));
    }
    return ResultV2<std::vector<McpPromptMessage>>::ok(std::move(messages));
}

// ============================================================
// 内部：JSON-RPC 收发
// ============================================================

ResultV2<nlohmann::json> McpClient::raw_request(const std::string& method,
                                                const nlohmann::json& params, int timeout_ms) {
    nlohmann::json msg = {{"jsonrpc", "2.0"}, {"id", m_next_id++}, {"method", method}};
    if (params.is_object()) {
        msg["params"] = params;
    }
    // 2.0 无状态模式：每请求注入协议版本
    if (m_stateless) {
        if (!msg.contains("params") || !msg["params"].is_object()) {
            msg["params"] = nlohmann::json::object();
        }
        msg["params"]["_meta"] = {{"io.modelcontextprotocol/protocolVersion", m_protocol_version}};
    }
    return m_transport->send_request(msg, timeout_ms);
}

ResultV2<nlohmann::json> McpClient::request(const std::string& method, const nlohmann::json& params,
                                            int timeout_ms) {
    auto resp = raw_request(method, params, timeout_ms);
    if (resp.is_err()) return resp.error();
    const auto& r = resp.value();
    if (r.contains("error")) {
        return ResultV2<nlohmann::json>::err(error_from_rpc(r.at("error"), method));
    }
    if (!r.contains("result")) {
        return ResultV2<nlohmann::json>::err(Error::Code::InternalError, "MCP 响应缺少 result",
                                             "method=" + method);
    }
    return ResultV2<nlohmann::json>::ok(r.at("result"));
}

ResultV2<void> McpClient::notify(const std::string& method, const nlohmann::json& params) {
    nlohmann::json msg = {{"jsonrpc", "2.0"}, {"method", method}};
    if (params.is_object()) {
        msg["params"] = params;
    }
    return m_transport->send_notification(msg);
}

McpCacheMeta McpClient::extract_cache_meta(const nlohmann::json& result) {
    McpCacheMeta meta;
    if (!result.is_object()) return meta;
    const auto& m = result.value("_meta", nlohmann::json::object());
    if (m.is_object()) {
        meta.ttl_ms = m.value("ttlMs", 0);
        meta.cache_scope = m.value("cacheScope", "");
    }
    return meta;
}

ResultV2<nlohmann::json> McpClient::request_cached(const std::string& method,
                                                   const nlohmann::json& params, int timeout_ms) {
    const std::string key = method + "|" + params.dump();
    const auto now = std::chrono::steady_clock::now();
    auto it = m_cache.find(key);
    if (it != m_cache.end() && it->second.expires_at > now) {
        return ResultV2<nlohmann::json>::ok(it->second.result);
    }

    auto resp = request(method, params, timeout_ms);
    if (resp.is_err()) return resp.error();

    // 2.0 缓存：响应 result._meta.ttlMs > 0 时写入缓存（cacheScope 记录但不区分）
    const auto& result = resp.value();
    const auto meta = extract_cache_meta(result);
    if (meta.ttl_ms > 0) {
        m_cache[key] = CacheEntry{result, now + std::chrono::milliseconds(meta.ttl_ms)};
    }
    return resp;
}

ResultV2<nlohmann::json> McpClient::do_initialize(const std::string& version, int timeout_ms) {
    nlohmann::json params = {{"protocolVersion", version},
                             {"capabilities", nlohmann::json::object()},
                             {"clientInfo", {{"name", "workx"}, {"version", "0.5.x"}}}};
    auto resp = raw_request("initialize", params, timeout_ms);
    if (resp.is_err()) return resp.error();
    const auto& r = resp.value();
    if (r.contains("error")) {
        // 标记协议版本不匹配，供 connect 决定是否重试
        if (is_unsupported_protocol_version(r.at("error"))) {
            return ResultV2<nlohmann::json>::err(Error::Code::ToolExecutionFailed,
                                                 "Unsupported protocol version",
                                                 "unsupported_protocol");
        }
        return ResultV2<nlohmann::json>::err(error_from_rpc(r.at("error"), "initialize"));
    }
    if (!r.contains("result")) {
        return ResultV2<nlohmann::json>::err(
            Error::Code::InternalError, "initialize 响应缺少 result", "McpClient::do_initialize");
    }
    return ResultV2<nlohmann::json>::ok(r.at("result"));
}

}  // namespace agent::mcp
