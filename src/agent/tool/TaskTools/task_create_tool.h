/**
 * @file task_create_tool.h
 * @brief TaskCreateTool — 创建任务（V2 细粒度，对齐 cc TaskCreateTool）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief TaskCreateTool — 创建新任务，返回自增 id
class TaskCreateTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
