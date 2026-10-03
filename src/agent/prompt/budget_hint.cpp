/**
 * @file budget_hint.cpp
 * @brief 剩余预算提示（Issue #83）实现
 */

#include "agent/prompt/budget_hint.h"

#include <format>

namespace agent::prompt {

namespace {

/// @brief Warn 档阈值（百分比）。与 LangChain 的 time-budget-warning 同思路：
///        留出足够轮次让模型跑一次验证并组织收尾答复，而不是在最后一轮才喊停。
constexpr int kWarnPercent = 30;
/// @brief Critical 档阈值（百分比）。低于此值已不足以完成一次「修改 → 验证」往返。
constexpr int kCriticalPercent = 10;

}  // namespace

BudgetTier budget_tier(int remaining, int total) {
    // 退化输入（未配置预算 / 预算已耗尽）统一按 Normal：
    // 耗尽的情形由 ReAct 主循环的达上限评审负责，本模块不再叠加一条提示。
    if (total <= 0 || remaining <= 0) {
        return BudgetTier::Normal;
    }
    const int percent = remaining * 100 / total;
    if (percent <= kCriticalPercent) {
        return BudgetTier::Critical;
    }
    if (percent <= kWarnPercent) {
        return BudgetTier::Warn;
    }
    return BudgetTier::Normal;
}

std::string format_duration(double seconds) {
    const auto total_seconds = seconds > 0.0 ? static_cast<long long>(seconds) : 0LL;
    const long long hours = total_seconds / 3600;
    const long long minutes = (total_seconds % 3600) / 60;
    const long long secs = total_seconds % 60;
    if (hours > 0) {
        return std::format("{}h{}m{}s", hours, minutes, secs);
    }
    if (minutes > 0) {
        return std::format("{}m{}s", minutes, secs);
    }
    return std::format("{}s", secs);
}

std::string format_budget_hint(int remaining, int total, double elapsed_seconds,
                               std::string_view verify_command) {
    const BudgetTier tier = budget_tier(remaining, total);
    if (tier == BudgetTier::Normal) {
        return {};
    }

    std::string hint = std::format("[预算提醒] 剩余轮次 {} / {}（已用时 {}）。", remaining, total,
                                   format_duration(elapsed_seconds));

    if (tier == BudgetTier::Critical) {
        // 验收标准要求：剩余 < 10% 时要的是「当前进展 + 未完成项」，
        // 而不是再跑一遍测试 —— 此处刻意不再提验证命令，避免与立即交卷互相打架。
        hint +=
            "基础预算已不足 10%：立即停止任何新的改动，把当前已完成的部分"
            "与尚未完成的清单整理成最终答复输出，不要再调用工具开始新的修改。";
        return hint;
    }

    hint += "基础预算已不足 30%：停止新增改动与范围扩展，转入验证与收尾——";
    if (verify_command.empty()) {
        hint += "运行项目的构建/测试命令确认当前成果，然后给出最终答复。";
    } else {
        hint += std::format("先运行 `{}` 确认当前成果，再给出最终答复；不要另起新的修改方向。",
                            verify_command);
    }
    return hint;
}

}  // namespace agent::prompt
