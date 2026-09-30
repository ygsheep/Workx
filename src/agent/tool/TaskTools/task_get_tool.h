/**
 * @file task_get_tool.h
 * @brief TaskGetTool — 读取单个任务（V2，对齐 cc TaskGetTool）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief TaskGetTool — 按 taskId 读取任务；不存在返回 null（非错误）
class TaskGetTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
