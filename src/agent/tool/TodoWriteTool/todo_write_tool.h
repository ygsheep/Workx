/**
 * @file todo_write_tool.h
 * @brief TodoWriteTool — 会话待办清单全量更新工具（V1，对齐 cc TodoWriteTool）
 * @details AI 主动创建/更新待办清单，全量替换语义。每次调用携带完整清单，
 *          全部 completed 时置空。变更经 TodoStore 发布 TodoUpdatedEvent 推送到 UI。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief TodoWriteTool — 全量更新当前会话待办清单
class TodoWriteTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
