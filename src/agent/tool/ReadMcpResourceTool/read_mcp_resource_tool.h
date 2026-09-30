/**
 * @file read_mcp_resource_tool.h
 * @brief ReadMcpResourceTool — 读取 MCP 资源内容（Issue #27）
 * @details 读取指定 MCP server 的指定资源（uri）内容。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>

#include "agent/mcp/mcp_client_manager.h"
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief ReadMcpResourceTool — 读取 MCP 资源
class ReadMcpResourceTool : public ITool {
   public:
    explicit ReadMcpResourceTool(std::shared_ptr<mcp::McpClientManager> manager);

    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

    /// @brief 读取外部 MCP 资源需用户确认（P2-7：Default 模式不静默读取）
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

   private:
    std::shared_ptr<mcp::McpClientManager> m_manager;
};

}  // namespace agent::tool
