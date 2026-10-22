/**
 * @file send_message_tool.h
 * @brief SendMessageTool — 向运行中后台任务投递补充指令
 * @details #52 阶段 C：父 Agent 中途给运行中的子 Agent 补充指令；
 *          消息入 Task 的 pending 队列，子 Agent 在工具轮边界 drain 注入上下文
 *          （对齐 cc SendMessage 语义：仅运行中的直接子任务可收消息）
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief SendMessageTool — 按 task_id 向运行中任务投递一条补充指令
///
/// 错误分类沿用 TaskStop 惯例（#52 阶段 A）：
/// not_found（目标不存在）/ not_running（已结束，去 TaskOutput 读结果）/
/// unsupported_type（Critical 系统任务不可投递）。
class SendMessageTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
