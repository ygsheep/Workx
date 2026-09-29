/**
 * @file exit_plan_mode_v2_tool.h
 * @brief ExitPlanModeV2Tool — 退出计划模式工具（#28）
 * @details AI 完成规划后调用：把方案呈现给用户确认（复用 AskUserRequestEvent
 *          确认通道），用户批准后恢复进入计划前的原权限模式（评审 #1：非硬编码
 *          回 Default），并发布 ExitPlanModeEvent 通知宿主。未批准则保持计划模式。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief 退出计划模式工具
class ExitPlanModeV2Tool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool