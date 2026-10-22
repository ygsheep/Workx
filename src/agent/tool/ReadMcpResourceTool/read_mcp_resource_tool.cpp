/**
 * @file read_mcp_resource_tool.cpp
 * @brief ReadMcpResourceTool 实现
 * @details 调用 resources/read 读取指定资源内容：
 *          - 文本内容（text）直接输出
 *          - 二进制内容（blob，base64）标注长度，不直接解码
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/ReadMcpResourceTool/read_mcp_resource_tool.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <sstream>
#include <unordered_set>

#include "agent/api/remote/http_client.h"
#include "agent/api/remote/ssrf.h"
#include "agent/tool/permission_ask.h"

namespace agent::tool {

namespace {

/// @brief 校验 MCP 资源 URI 安全性（SSRF / 本地文件读取防护）
/// @details 拒绝危险 scheme（file/gopher/data 等）与解析到内网/回环的 http(s) 地址；
///          自定义 scheme（docs://、repo:// 等）由 MCP server 解释，不构成客户端 SSRF。
ResultV2<void> validate_resource_uri(const std::string& uri) {
    const auto colon = uri.find(':');
    if (colon == std::string::npos || colon == 0) {
        return ResultV2<void>::err(Error::Code::InvalidInput, "MCP 资源 URI 缺少合法 scheme",
                                   "uri=" + uri);
    }
    std::string scheme = uri.substr(0, colon);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::unordered_set<std::string> kDangerousSchemes = {
        "file", "gopher", "data", "javascript", "vbscript", "ftp", "dict",
        "ldap", "jar",    "ws",   "wss",        "telnet",   "ssh", "smb",
    };
    if (kDangerousSchemes.count(scheme) > 0) {
        return ResultV2<void>::err(Error::Code::PermissionDenied,
                                   "MCP 资源 URI scheme 被禁止: " + scheme, "uri=" + uri);
    }

    // http(s)：校验主机不解析到内网/回环/链路本地（SSRF）
    if (scheme == "http" || scheme == "https") {
        const auto purl = HttpClient::parse_url(uri);
        if (purl.host.empty()) {
            return ResultV2<void>::err(Error::Code::InvalidInput, "MCP 资源 URI 缺少主机名",
                                       "uri=" + uri);
        }
        if (host_resolves_to_private(purl.host)) {
            return ResultV2<void>::err(Error::Code::PermissionDenied,
                                       "MCP 资源 URI 解析到内网/回环/链路本地地址，已拦截（SSRF）",
                                       "uri=" + uri);
        }
    }
    return ResultV2<void>::ok();
}

}  // namespace

ReadMcpResourceTool::ReadMcpResourceTool(std::shared_ptr<mcp::McpClientManager> manager)
    : m_manager(std::move(manager)) {}

const std::string& ReadMcpResourceTool::name() const {
    static const std::string n{"ReadMcpResourceTool"};
    return n;
}

const std::string& ReadMcpResourceTool::description() const {
    static const std::string d{"Reads a specific MCP resource by URI from a connected MCP server."};
    return d;
}

const std::string& ReadMcpResourceTool::prompt() const {
    static const std::string p{
        "ReadMcpResourceTool：读取指定 MCP server 的资源内容。\n"
        "用法：{\"server\": \"<server名>\", \"uri\": \"<资源URI>\"}\n"
        "uri 来自 ListMcpResourcesTool 的输出。"};
    return p;
}

nlohmann::json ReadMcpResourceTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"server", {{"type", "string"}, {"description", "MCP server name"}}},
              {"uri", {{"type", "string"}, {"description", "Resource URI to read"}}}}},
            {"required", {"server", "uri"}},
            {"additionalProperties", false}};
}

PermissionResult ReadMcpResourceTool::check_permissions(const nlohmann::json& input,
                                                        const ToolContext& ctx) const {
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    if (!input.contains("server") || !input.at("server").is_string()) {
        return PermissionResult::ok();
    }
    const std::string server = input.at("server").get<std::string>();
    if (ask_user_confirm(ctx, std::format("需要从 MCP server '{}' 读取资源，请确认：\n\n"
                                          "允许读取该 MCP server 的资源？",
                                          server))) {
        return PermissionResult::ok();
    }
    return PermissionResult::err(Error::Code::PermissionDenied,
                                 "用户拒绝读取 MCP server: " + server);
}

ResultV2<ToolResult> ReadMcpResourceTool::call(const nlohmann::json& input,
                                               const ToolContext& /*ctx*/
) const {
    if (!input.contains("server") || !input.at("server").is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "ReadMcpResourceTool 需要字符串参数 server");
    }
    if (!input.contains("uri") || !input.at("uri").is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "ReadMcpResourceTool 需要字符串参数 uri");
    }
    if (!m_manager) {
        return ResultV2<ToolResult>::err(Error::Code::InternalError, "MCP 连接管理器未初始化");
    }

    const std::string server = input.at("server").get<std::string>();
    const std::string uri = input.at("uri").get<std::string>();

    // SSRF/本地文件读取防护：校验 URI scheme 与 http(s) 主机
    auto uri_check = validate_resource_uri(uri);
    if (uri_check.is_err()) {
        return ResultV2<ToolResult>::err(
            Error::Code::PermissionDenied,
            "ReadMcpResourceTool 拒绝读取不安全资源: " + uri_check.error().message,
            "server=" + server + "; uri=" + uri);
    }

    auto client = m_manager->get_client(server);
    if (!client) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         "MCP server 不存在或未连接: " + server);
    }

    auto result = client->read_resource(uri);
    if (result.is_err()) {
        return ResultV2<ToolResult>::err(Error::Code::ToolExecutionFailed,
                                         "读取 MCP 资源失败: " + result.error().message,
                                         "server=" + server + "; uri=" + uri);
    }

    std::ostringstream out;
    out << "# MCP 资源内容: " << server << " / " << uri << "\n";
    const auto& contents = result.value();
    if (contents.empty()) {
        out << "（资源为空）\n";
    }
    for (const auto& c : contents) {
        if (!c.mime_type.empty()) {
            out << "mimeType: " << c.mime_type << "\n";
        }
        if (!c.text.empty()) {
            out << "---\n" << c.text << "\n";
        } else if (!c.blob.empty()) {
            out << "[二进制内容] blob_len=" << c.blob.size() << "（base64，未解码）\n";
        }
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(out.str()));
}

}  // namespace agent::tool
