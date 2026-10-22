/**
 * @file test_budget_hint.cpp
 * @brief 剩余预算提示单元测试（Issue #83）
 * @details 全部覆盖 prompt::budget_tier / format_duration / format_budget_hint 三个纯函数。
 *          档位阈值（30% / 10%）用整除边界逐点钉死 —— 这两个数字一旦漂移，
 *          「什么时候开始收尾」的行为就变了，必须有用例挡住。
 *          金额的 `$Y` 维度依赖 #55 的费用累计，本模块未接入，故无对应断言。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

#include "agent/prompt/budget_hint.h"

namespace {

using agent::prompt::budget_tier;
using agent::prompt::BudgetTier;
using agent::prompt::format_budget_hint;
using agent::prompt::format_duration;

/// @brief 在 hints 中找是否出现过含子串的条目（便于断言"没有更早触发"）
bool any_contains(const std::string& text, std::string_view needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

// ============================================================
// 档位判定：30% / 10% 两条阈值
// ============================================================

TEST_CASE("budget_tier 阈值边界：>30% 为 Normal，30% 落入 Warn", "[issue83][budget_hint]") {
    CHECK(budget_tier(13, 40) == BudgetTier::Normal);  // 32.5%
    CHECK(budget_tier(12, 40) == BudgetTier::Warn);    // 30.0%
    CHECK(budget_tier(3, 10) == BudgetTier::Warn);     // 30.0%
}

TEST_CASE("budget_tier 阈值边界：11% 落入 Warn，10% 落入 Critical", "[issue83][budget_hint]") {
    CHECK(budget_tier(5, 40) == BudgetTier::Warn);      // 12.5%
    CHECK(budget_tier(11, 100) == BudgetTier::Warn);    // 11%
    CHECK(budget_tier(4, 40) == BudgetTier::Critical);  // 10.0%
    CHECK(budget_tier(10, 100) == BudgetTier::Critical);
    CHECK(budget_tier(1, 10) == BudgetTier::Critical);
}

TEST_CASE("budget_tier 退化输入不产生提示", "[issue83][budget_hint]") {
    // 预算耗尽（<=0）交给主循环的达上限评审处理，本模块不叠加提示
    CHECK(budget_tier(0, 40) == BudgetTier::Normal);
    CHECK(budget_tier(-1, 40) == BudgetTier::Normal);
    CHECK(budget_tier(10, 0) == BudgetTier::Normal);
}

TEST_CASE("budget_tier 满预算为 Normal", "[issue83][budget_hint]") {
    CHECK(budget_tier(40, 40) == BudgetTier::Normal);
    CHECK(budget_tier(1, 1) == BudgetTier::Normal);  // 100%
}

// ============================================================
// 提示文案
// ============================================================

TEST_CASE("format_budget_hint 在 Normal 档返回空串（不污染上下文）", "[issue83][budget_hint]") {
    CHECK(format_budget_hint(13, 40, 12.0, "").empty());
    CHECK(format_budget_hint(40, 40, 0.0, "ctest").empty());
    CHECK(format_budget_hint(0, 40, 5.0, "").empty());
}

TEST_CASE("Warn 档文案带剩余轮次与总预算", "[issue83][budget_hint]") {
    const std::string hint = format_budget_hint(3, 10, 12.0, "");
    CHECK(any_contains(hint, "剩余轮次 3 / 10"));
    CHECK(any_contains(hint, "12s"));
}

TEST_CASE("Warn 档无可用验证命令时降级为泛化要求", "[issue83][budget_hint]") {
    const std::string hint = format_budget_hint(12, 40, 30.0, "");
    CHECK(any_contains(hint, "构建/测试命令"));
    CHECK_FALSE(any_contains(hint, "`"));
}

TEST_CASE("Warn 档有验证命令时点名该命令", "[issue83][budget_hint]") {
    const std::string hint = format_budget_hint(12, 40, 30.0, "ctest --output-on-failure");
    CHECK(any_contains(hint, "`ctest --output-on-failure`"));
    CHECK_FALSE(any_contains(hint, "构建/测试命令"));
}

TEST_CASE("Critical 档要求立即交卷且不再要求跑测试", "[issue83][budget_hint]") {
    const std::string hint = format_budget_hint(1, 10, 200.0, "ctest --output-on-failure");
    CHECK(any_contains(hint, "剩余轮次 1 / 10"));
    CHECK(any_contains(hint, "已完成的部分"));
    CHECK(any_contains(hint, "尚未完成"));
    // 剩余 10% 已不足以支撑一次「修改 → 验证」往返，点名命令只会误导模型
    CHECK_FALSE(any_contains(hint, "ctest --output-on-failure"));
    CHECK_FALSE(any_contains(hint, "构建/测试命令"));
}

// ============================================================
// 时长格式化
// ============================================================

TEST_CASE("format_duration 按量级降位到秒/分/时", "[issue83][budget_hint]") {
    CHECK(format_duration(0.0) == "0s");
    CHECK(format_duration(-5.0) == "0s");
    CHECK(format_duration(45.4) == "45s");
    CHECK(format_duration(200.9) == "3m20s");
    CHECK(format_duration(3725.0) == "1h2m5s");
}
