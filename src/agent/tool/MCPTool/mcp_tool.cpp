/**
 * @file mcp_tool.cpp
 * @brief MCPTool 实现（Issue #27）
 * @details 分发式调用：按 server 名从 McpClientManager 获取 client，
 *          调用 tools/call 并规范化结果（content[] → 文本）。
 *          权限：Default 模式下 AskUser 确认外部工具调用。
 * @version 2.0.0
 * @date 2026-08
 */

#include "agent/tool/MCPTool/mcp_tool.h"

#include <format>

#include "agent/tool/permission_ask.h"

namespace agent::tool {

MCPTool::MCPTool(std::shared_ptr<mcp::McpClientManager> manager) : m_manager(std::move(manager)) {}

const std::string& MCPTool::name() const {
    static const std::string n{"MCP"};
    return n;
}

const std::string& MCPTool::description() const {
    static const std::string d{
        "Invokes an external tool via Model Context Protocol (MCP). "
        "Calls tools exposed by connected MCP servers."};
    return d;
}

const std::string& MCPTool::prompt() const {
    static const std::string base{
        "MCP 工具：调用已连接的 MCP server 暴露的外部工具。\n"
        "用法：{\"server\": \"<server名>\", \"tool\": \"<工具名>\", \"input\": {...}}\n"
        "input 为传给外部工具的参数对象。\n"};

    if (!m_manager || m_manager->empty()) {
        static const std::string no_server = base + "当前没有已连接的 MCP server。";
        return no_server;
    }
    static thread_local std::string cached;
    cached = base + "当前已连接 server：\n" + m_manager->describe_servers();
    return cached;
}

nlohmann::json MCPTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"server", {{"type", "string"}, {"description", "MCP server name"}}},
              {"tool", {{"type", "string"}, {"description", "Tool name on the MCP server"}}},
              {"input", {{"type", "object"}, {"description", "Tool input parameters"}}}}},
            {"required", {"server", "tool"}},
            {"additionalProperties", false}};
}

PermissionResult MCPTool::check_permissions(const nlohmann::json& input,
                                            const ToolContext& ctx) const {
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    if (!input.contains("server") || !input.at("server").is_string()) {
        return PermissionResult::ok();
    }
    const std::string server = input.at("server").get<std::string>();
    if (ask_user_confirm(ctx, std::format("MCP 工具需要调用外部 server '{}' 的工具，请确认：\n\n"
                                          "允许调用该 MCP server？",
                                          server))) {
        return PermissionResult::ok();
    }
    return PermissionResult::err(Error::Code::PermissionDenied,
                                 "用户拒绝调用 MCP server: " + server);
}

ResultV2<ToolResult> MCPTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    if (!input.contains("server") || !input.at("server").is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput, "MCP 需要字符串参数 server",
                                         input.dump());
    }
    if (!input.contains("tool") || !input.at("tool").is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput, "MCP 需要字符串参数 tool",
                                         input.dump());
    }
    // #56 方案 D：优先解析当前作用域的 MCP 管理器（子 Agent 的 inline/引用 server），
    //             否则回退到构造时的全局管理器（父会话 MCP server）。
    mcp::McpClientManager* manager = ctx.mcp_manager_ptr ? ctx.mcp_manager_ptr : m_manager.get();
    if (!manager) {
        return ResultV2<ToolResult>::err(Error::Code::InternalError, "MCP 连接管理器未初始化");
    }

    const std::string server = input.at("server").get<std::string>();
    const std::string tool = input.at("tool").get<std::string>();
    const nlohmann::json args = input.contains("input") && input.at("input").is_object()
                                    ? input.at("input")
                                    : nlohmann::json::object();

    auto client = manager->get_client(server);
    if (!client) {
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound, "MCP server 不存在或未连接: " + server,
            "可用 server: " + [&] {
                std::string names;
                for (const auto& n : manager->server_names()) {
                    if (!names.empty()) names += ", ";
                    names += n;
                }
                return names.empty() ? "无" : names;
            }());
    }

    // P1-6：工具存在性校验（LLM 可能调用已移除/隐藏的工具）
    if (!manager->has_tool(server, tool)) {
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound, "MCP server '" + server + "' 未暴露工具 '" + tool + "'",
            "可用工具: " + [&] {
                std::string names;
                if (auto c = manager->get_client(server)) {
                    auto tools = c->list_tools();
                    if (tools.is_ok()) {
                        for (const auto& t : tools.value()) {
                            if (!names.empty()) names += ", ";
                            names += t.name;
                        }
                    }
                }
                return names.empty() ? "无" : names;
            }());
    }

    auto result = client->call_tool(tool, args);
    if (result.is_err()) {
        return ResultV2<ToolResult>::err(Error::Code::ToolExecutionFailed,
                                         "MCP 工具调用失败: " + result.error().message,
                                         "server=" + server + "; tool=" + tool);
    }

    const auto& call = result.value();
    std::ostringstream out;
    out << "# MCP 工具结果: " << server << " / " << tool << "\n";
    if (call.is_error) {
        out << "（server 返回错误）\n";
    }
    for (const auto& c : call.content) {
        if (c.type == "text") {
            out << c.text << "\n";
        } else if (c.type == "image") {
            out << "[图片] mimeType=" << c.mime_type << " data_len=" << c.data.size() << "\n";
        } else {
            out << "[资源] type=" << c.type << "\n";
        }
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(out.str()));
}

}  // namespace agent::tool
