/**
 * @file list_mcp_resources_tool.cpp
 * @brief ListMcpResourcesTool 实现
 * @details 遍历所有（或指定）已连接 MCP server，调用 resources/list，
 *          输出 uri/name/mimeType/description 列表。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/ListMcpResourcesTool/list_mcp_resources_tool.h"

#include <sstream>

namespace agent::tool {

ListMcpResourcesTool::ListMcpResourcesTool(std::shared_ptr<mcp::McpClientManager> manager)
    : m_manager(std::move(manager)) {}

const std::string& ListMcpResourcesTool::name() const {
    static const std::string n{"ListMcpResourcesTool"};
    return n;
}

const std::string& ListMcpResourcesTool::description() const {
    static const std::string d{
        "Lists resources exposed by connected MCP servers. "
        "Optionally filter by server name."};
    return d;
}

const std::string& ListMcpResourcesTool::prompt() const {
    static const std::string p{
        "ListMcpResourcesTool：列出已连接 MCP server 暴露的资源（Resources）。\n"
        "用法：{\"server\": \"<server名>\"}（可选，省略则列出所有 server）\n"
        "返回每个资源的 uri / name / mimeType / description，供后续 ReadMcpResourceTool 读取。"};
    return p;
}

nlohmann::json ListMcpResourcesTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"server",
               {{"type", "string"},
                {"description", "可选：按 server 名过滤（省略则列出所有 server）"}}}}},
            {"required", nlohmann::json::array()},
            {"additionalProperties", false}};
}

ResultV2<ToolResult> ListMcpResourcesTool::call(const nlohmann::json& input,
                                                const ToolContext& /*ctx*/
) const {
    if (!m_manager) {
        return ResultV2<ToolResult>::err(Error::Code::InternalError, "MCP 连接管理器未初始化");
    }

    const std::string target = input.contains("server") && input.at("server").is_string()
                                   ? input.at("server").get<std::string>()
                                   : "";

    std::vector<std::shared_ptr<mcp::McpClient>> clients;
    if (!target.empty()) {
        auto client = m_manager->get_client(target);
        if (!client) {
            return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                             "MCP server 不存在或未连接: " + target);
        }
        clients.push_back(std::move(client));
    } else {
        clients = m_manager->clients();
    }

    if (clients.empty()) {
        return ResultV2<ToolResult>::ok(
            ToolResult::ok(std::string("当前没有已连接的 MCP server。")));
    }

    std::ostringstream out;
    bool any = false;
    for (const auto& client : clients) {
        auto result = client->list_resources();
        if (result.is_err()) {
            out << "## " << client->name() << "（错误: " << result.error().message << "）\n";
            continue;
        }
        const auto& resources = result.value();
        out << "## " << client->name() << "（" << resources.size() << " 个资源）\n";
        for (const auto& r : resources) {
            out << "- uri: " << r.uri << "\n";
            out << "  name: " << (r.name.empty() ? "-" : r.name) << "\n";
            if (!r.mime_type.empty()) out << "  mimeType: " << r.mime_type << "\n";
            if (!r.description.empty()) out << "  description: " << r.description << "\n";
        }
        any = true;
    }
    if (!any) {
        out << "（无资源）\n";
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(out.str()));
}

}  // namespace agent::tool
