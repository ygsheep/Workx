/**
 * @file powershell_tool.h
 * @brief PowerShellTool — Windows PowerShell 执行工具
 * @details 专为 Windows 平台设计的 PowerShell 命令执行工具，与 BashTool 并存：
 *          - Windows 上同时注册 BashTool（cmd.exe）和 PowerShellTool（powershell.exe）
 *          - 由模型根据任务特征自行选用（Git/构建/系统管理用 PowerShell，
 *            已有 Unix 风格脚本用 Bash）
 *          - 非 Windows 平台不注册
 *
 *          支持特性（对齐 BashTool）：
 *          - 同步执行（默认）+ 后台执行（run_in_background=true）
 *          - 沙盒包装 + 超时 + 取消回调
 *          - 进度上报
 *
 *          对齐 Claude Code 的 PowerShellTool，但简化为：
 *          - 不实现 AST 安全解析（后续迭代）
 *          - 不实现 dangerous cmdlet 黑名单（后续迭代）
 *          - 版本检测（5.1 vs 7+）暂不实现，prompt 统一按 5.1 兼容写法
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief PowerShellTool — Windows PowerShell 执行工具
/// @details 仅 Windows 平台可用，非 Windows 平台不注册
class PowerShellTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    /// @brief 权限检查（#36：Bypass 放行 / Plan 拒绝执行 / 危险命令 AskUser 确认）
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    /// 同步执行路径
    ResultV2<ToolResult> execute_sync(const std::string& command, const std::string& cwd,
                                      int timeout_ms, bool disable_sandbox,
                                      const ToolContext& ctx) const;

    /// 后台执行路径（通过 TaskManager）
    ResultV2<ToolResult> execute_background(const std::string& command, const std::string& cwd,
                                            int timeout_ms, bool disable_sandbox,
                                            const ToolContext& ctx) const;
};

}  // namespace agent::tool
