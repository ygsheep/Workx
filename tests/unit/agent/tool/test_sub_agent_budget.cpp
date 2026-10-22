/**
 * @file test_sub_agent_budget.cpp
 * @brief Issue #79：子 Agent 派生预算（SubAgentBudget）单元测试
 * @details 覆盖两级护栏（单次批量规模 / run 累计总量）与并发安全性。
 *          防递归由 AgentTool 侧保证（子 Agent 不含 Agent 工具），本预算只管规模。
 */

#include <atomic>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "agent/tool/context.h"

using agent::tool::SubAgentBudget;
using agent::tool::SubAgentReject;

TEST_CASE("SubAgentBudget rejects batch larger than max_batch", "[sub_agent_budget]") {
    SubAgentBudget budget(/*max_batch=*/5, /*max_total=*/100);
    SubAgentReject reject = SubAgentReject::None;

    REQUIRE_FALSE(budget.try_reserve(6, reject));
    REQUIRE(reject == SubAgentReject::BatchLimit);
    // 被拒不得扣减额度，否则错误信息里的剩余额度会失真
    REQUIRE(budget.launched() == 0);
}

TEST_CASE("SubAgentBudget allows batch within limits and deducts quota", "[sub_agent_budget]") {
    SubAgentBudget budget(5, 10);
    SubAgentReject reject = SubAgentReject::None;

    REQUIRE(budget.try_reserve(3, reject));
    REQUIRE(reject == SubAgentReject::None);
    REQUIRE(budget.launched() == 3);
    REQUIRE(budget.remaining() == 7);
}

TEST_CASE("SubAgentBudget rejects when cumulative total exceeds max_total", "[sub_agent_budget]") {
    SubAgentBudget budget(5, 10);
    SubAgentReject reject = SubAgentReject::None;

    REQUIRE(budget.try_reserve(5, reject));
    REQUIRE(budget.try_reserve(5, reject));
    REQUIRE(budget.launched() == 10);

    REQUIRE_FALSE(budget.try_reserve(1, reject));
    REQUIRE(reject == SubAgentReject::TotalLimit);
    REQUIRE(budget.launched() == 10);
    REQUIRE(budget.remaining() == 0);
}

TEST_CASE("SubAgentBudget treats non-positive limit as unlimited", "[sub_agent_budget]") {
    SubAgentBudget budget(/*max_batch=*/0, /*max_total=*/0);
    SubAgentReject reject = SubAgentReject::None;

    REQUIRE(budget.try_reserve(1000, reject));
    REQUIRE(reject == SubAgentReject::None);
    REQUIRE(budget.remaining() == -1);  // -1 表示不限
}

/// @brief 并发安全：多线程同时预约时，累计派生数绝不允许突破 max_total
TEST_CASE("SubAgentBudget never over-issues under concurrent reserve", "[sub_agent_budget]") {
    SubAgentBudget budget(/*max_batch=*/1000, /*max_total=*/100);
    std::atomic<int> success_count{0};
    std::vector<std::thread> threads;

    // 20 个线程各预约 10 个，总上限 100 → 恰好 10 次成功
    for (int i = 0; i < 20; ++i) {
        threads.emplace_back([&budget, &success_count]() {
            SubAgentReject r = SubAgentReject::None;
            if (budget.try_reserve(10, r)) {
                success_count.fetch_add(1);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(success_count.load() == 10);
    REQUIRE(budget.launched() == 100);
}
