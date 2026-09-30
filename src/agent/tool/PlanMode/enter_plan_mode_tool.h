/**
 * @file enter_plan_mode_tool.h
 * @brief EnterPlanModeTool — 进入计划模式工具（#28）
 * @details AI 处理大型/不确定任务时主动请求进入计划模式：
 *          1. 通过 ToolContext 回调把会话权限模式切换为 Plan
 *             （写文件/执行命令类工具被 check_permissions 拒绝）
 *          2. 发布 EnterPlanModeEvent 通知宿主（TUI）展示计划模式状态
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief 进入计划模式工具
class EnterPlanModeTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool