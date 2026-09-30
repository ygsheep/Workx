/**
 * @file BriefTool.h
 * @brief BriefTool — 强制用户通信通道（Issue #56 方案 B）
 * @details 复用 AskUser 的事件通道（AskUserRequestEvent + ChoicePanel 模态），
 *          提供模型向用户发起"工作确认/简短询问"的专用工具：
 *          - status 区分 proactive（默认，需先征得用户确认再继续）与 normal（普通询问）
 *          - 强制语义由双重机制保证：① 模态阻塞（用户响应前模型无法继续）；
 *            ② 工具 prompt() 声明"用户交互必须经 BriefTool 通道"的系统约定
 *          - 输出: {"status":"submitted"|"cancelled"|"timeout", "answer":"..."}
 * @version 1.0.0
 * @date 2026-09
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief BriefTool — 强制用户通信通道
/// @details 复用 AskUserRequestEvent 模态通道。工作线程发布事件后阻塞等待
///          result_promise（含超时），用户确认/答复通过 ChoicePanel 回填。
///          相比 AskUserTool：输入为单个 question + status 语义，专门用于
///          "开工前确认 / 临门一脚确认"等需要强制用户把关的场景。
class BriefTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
