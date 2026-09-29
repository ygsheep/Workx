/**
 * @file ask_user_config.cpp
 * @brief AskUser 输入校验实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/AskUser/ask_user_config.h"

namespace agent::tool {

namespace {

/// @brief 校验单个 question 节点（含 options 数组）
/// @details 与 tui::parse_choice_config 的成功路径一致：
///          question/header 为非空字符串，options 为 2-4 个含 label 的对象
bool validate_question(const nlohmann::json& q_j) {
    if (!q_j.contains("question") || !q_j["question"].is_string()) return false;
    if (q_j["question"].get<std::string>().empty()) return false;
    if (!q_j.contains("header") || !q_j["header"].is_string()) return false;
    if (!q_j.contains("options") || !q_j["options"].is_array()) return false;
    if (q_j["options"].empty()) return false;

    for (const auto& opt_j : q_j["options"]) {
        if (!opt_j.is_object()) return false;
        if (!opt_j.contains("label") || !opt_j["label"].is_string()) return false;
        if (opt_j["label"].get<std::string>().empty()) return false;
    }
    return true;
}

}  // namespace

bool validate_ask_user_json(const nlohmann::json& input) {
    try {
        if (!input.is_object()) return false;
        if (!input.contains("questions") || !input["questions"].is_array()) return false;
        if (input["questions"].empty()) return false;

        for (const auto& q_j : input["questions"]) {
            if (!q_j.is_object()) return false;
            if (!validate_question(q_j)) return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace agent::tool
