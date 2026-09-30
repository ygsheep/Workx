/**
 * @file AskUserTool.h
 * @brief AskUserTool — 向用户提问工具（多选/单选 + 自定义输入）
 * @details 通过事件总线发布 AskUserRequestEvent 触发 TUI ChoicePanel 模态，
 *          阻塞等待用户响应（含超时），结果通过 promise/future 回填。
 *          - 输入 schema 对齐 cc AskUserQuestionTool 的 questions 结构
 *          - 输出格式: {"status":"submitted"|"cancelled"|"timeout", "answers":{question: answer}}
 *          - 超时由 timeout_ms 参数控制（默认 300000ms = 5 分钟，0=不限时）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief AskUserTool — 向用户提问工具
///
/// 通过 EventBus 发布 AskUserRequestEvent，TUI 主循环收到后弹出 ChoicePanel，
/// 用户操作完成通过 result_promise->set_value() 回填，工具阻塞 future.wait_for()。
/// 超时自动返回 timeout 状态，避免工作线程永久阻塞。
class AskUserTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
