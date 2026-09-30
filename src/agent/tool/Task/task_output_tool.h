/**
 * @file task_output_tool.h
 * @brief TaskOutputTool — 读取后台任务输出工具
 * @details 读取 AgentTool 启动的子 Agent 等后台任务的输出与状态（对齐 TS TaskOutputTool 契约）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief TaskOutputTool — 按 task_id 读取后台任务输出
///
/// block=true（默认）时最多等待 timeout_ms 毫秒直到任务结束；
/// 任务不存在返回错误。
class TaskOutputTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    /// @brief 只读工具（无副作用）
    bool is_read_only() const override { return true; }

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool