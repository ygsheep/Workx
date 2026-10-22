/**
 * @file list_mcp_resources_tool.h
 * @brief ListMcpResourcesTool — 列出 MCP server 暴露的资源（Issue #27）
 * @details 列出所有（或指定）已连接 MCP server 的 Resources 列表。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>

#include "agent/mcp/mcp_client_manager.h"
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief ListMcpResourcesTool — 列出 MCP server 的资源
class ListMcpResourcesTool : public ITool {
   public:
    explicit ListMcpResourcesTool(std::shared_ptr<mcp::McpClientManager> manager);

    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    std::shared_ptr<mcp::McpClientManager> m_manager;
};

}  // namespace agent::tool
