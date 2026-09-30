/**
 * @file react_step_format.h
 * @brief ReAct 步骤格式化助手（子 Agent / 后台 Agent 共用，避免重复）
 * @details 将 ReActStep 格式化为任务输出缓冲的行文本 / 进度事件 step_type，
 *          供 AgentTool（SubAgent*）与 BackgroundAgent（Background*）一致渲染。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <format>
#include <string>

#include "agent/core/react_loop.h"  // ReActStep / ReActStepType

namespace agent {

/// @brief 格式化 ReAct 步骤为任务输出缓冲的行
inline std::string format_step_line(const ReActStep& step) {
    switch (step.type) {
        case ReActStepType::Thought:
            return step.thought_text.empty()
                       ? std::string{}
                       : std::format("[{}] Thought: {}", step.step_number, step.thought_text);
        case ReActStepType::Action:
            return std::format("[{}] Tool: {}", step.step_number, step.tool_name);
        case ReActStepType::Observation:
            return step.observation.empty()
                       ? std::string{}
                       : std::format("[{}] Observation: {}", step.step_number, step.observation);
        case ReActStepType::FinalAnswer:
            return step.thought_text.empty()
                       ? std::string{}
                       : std::format("[{}] Final: {}", step.step_number, step.thought_text);
    }
    return {};
}

/// @brief 步骤类型 → 进度事件 step_type 字符串
inline const char* step_type_str(ReActStepType type) {
    switch (type) {
        case ReActStepType::Thought:
            return "thought";
        case ReActStepType::Action:
            return "action";
        case ReActStepType::Observation:
            return "observation";
        case ReActStepType::FinalAnswer:
            return "final";
    }
    return "unknown";
}

}  // namespace agent