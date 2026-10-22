/**
 * @file ask_user_config.h
 * @brief AskUser 输入校验（宿主无关）
 * @details 校验 AskUser 工具的 questions JSON 结构（对齐 cc AskUserQuestionTool
 *          schema）。纯函数，不依赖任何 UI 层；TUI 侧渲染面板时使用自己的
 *          parse_choice_config 再次解析。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <nlohmann/json.hpp>

#include "core/export.h"

namespace agent::tool {

/// @brief 校验 AskUser 输入是否为合法的 questions 结构
/// @param input AskUser 工具完整输入 JSON（含 "questions" 键）
/// @return 合法返回 true
/// @details 校验规则（与 tui::parse_choice_config 的成功路径一致）：
///          - input 含 "questions" 数组且非空（1-4 个问题）
///          - 每个 question 含 "question"/"header"/"options"（2-4 个选项）
///          - 每个 option 含 "label"（非空字符串）
bool WORKX_API validate_ask_user_json(const nlohmann::json& input);

}  // namespace agent::tool
