/**
 * @file bash_tool.h
 * @brief BashTool — Shell 执行工具
 * @details 执行 shell 命令并返回输出。支持：
 *          - 同步执行（默认）：阻塞等待命令完成，返回 stdout/stderr/exit_code
 *          - 后台执行（run_in_background=true）：通过 TaskManager 异步执行，立即返回 task_id
 *          - 沙盒包装：默认通过 SandboxAdapter 包装命令（可禁用）
 *          - 超时设置：可配置超时毫秒数
 *          - 进度回调：通过 ctx.progress_callback 上报执行进度
 *
 *          对齐 Claude Code CLI 的 BashTool，但简化为：
 *          - 不实现图片输出检测/压缩
 *          - 不实现 sed 编辑预览
 *          - 不实现 AST 安全解析（后续迭代）
 *
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/ShellTool/sandbox_visibility.h"
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief BashTool — Shell 执行工具
class BashTool : public ITool {
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

    /// @brief #84：沙箱状态上报器（降级 / 显式关闭的审计各只留痕一次）
    mutable shell_common::SandboxVisibility sandbox_visibility_;
};

}  // namespace agent::tool
