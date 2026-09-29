/**
 * @file task_stop_tool.h
 * @brief TaskStopTool — 停止后台任务工具
 * @details 停止 AgentTool 启动的子 Agent 等后台任务（对齐 TS TaskStopTool 契约）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief TaskStopTool — 按 task_id 停止后台任务
///
/// 任务不存在返回错误；任务已结束返回错误（非运行态）。
class TaskStopTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool