/**
 * @file mcp_tool.h
 * @brief MCPTool — MCP 工具调用（Issue #27）
 * @details 通过 Model Context Protocol 调用外部工具（分发式）：
 *          - 输入 {server, tool, input}，运行时按 server 名分发到对应 MCP client
 *          - prompt() 动态注入已连接 server 及工具清单，供模型参考
 * @version 2.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>

#include "agent/mcp/mcp_client_manager.h"
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief MCPTool — MCP 工具调用（分发式）
///
/// 通过 Model Context Protocol 调用外部工具：
/// - 支持 MCP server 注册的工具
/// - 透传输入参数和返回结果
class MCPTool : public ITool {
   public:
    explicit MCPTool(std::shared_ptr<mcp::McpClientManager> manager);

    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    std::shared_ptr<mcp::McpClientManager> m_manager;
};

}  // namespace agent::tool
