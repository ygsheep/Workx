/**
 * @file task_update_tool.h
 * @brief TaskUpdateTool — 更新任务（V2 细粒度，对齐 cc TaskUpdateTool）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief TaskUpdateTool — 按 taskId 更新任务字段/状态；status=deleted 时删除
class TaskUpdateTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
