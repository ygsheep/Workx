/**
 * @file test_wall_clock_hint.cpp
 * @brief prompt::wall_clock_hint 单测（Issue #166 方案 C）
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/prompt/wall_clock_hint.h"

using namespace agent::prompt;

TEST_CASE("wall_clock_tier: 未启用墙钟一律 Normal", "[wall_clock][issue166]") {
    // reserve_sec <= 0 = 关闭：无论剩多少时间都不注入（交互式场景行为不变）
    REQUIRE(wall_clock_tier(0.0, 0) == WallClockTier::Normal);
    REQUIRE(wall_clock_tier(1.0, -5) == WallClockTier::Normal);
    REQUIRE(wall_clock_tier(10000.0, 0) == WallClockTier::Normal);
}

TEST_CASE("wall_clock_tier: 三档边界", "[wall_clock][issue166]") {
    constexpr int kReserve = 90;  // Critical 界 = 30s
    // 充裕：> reserve
    REQUIRE(wall_clock_tier(91.0, kReserve) == WallClockTier::Normal);
    REQUIRE(wall_clock_tier(1000.0, kReserve) == WallClockTier::Normal);
    // Warn：(reserve/3, reserve]
    REQUIRE(wall_clock_tier(90.0, kReserve) == WallClockTier::Warn);
    REQUIRE(wall_clock_tier(45.0, kReserve) == WallClockTier::Warn);
    REQUIRE(wall_clock_tier(30.1, kReserve) == WallClockTier::Warn);
    // Critical：<= reserve/3（含已越过 deadline 的负值）
    REQUIRE(wall_clock_tier(30.0, kReserve) == WallClockTier::Critical);
    REQUIRE(wall_clock_tier(1.0, kReserve) == WallClockTier::Critical);
    REQUIRE(wall_clock_tier(0.0, kReserve) == WallClockTier::Critical);
    REQUIRE(wall_clock_tier(-12.0, kReserve) == WallClockTier::Critical);
}

TEST_CASE("format_wall_clock_hint: Normal 档不产文本", "[wall_clock][issue166]") {
    REQUIRE(format_wall_clock_hint(500.0, WallClockTier::Normal, "").empty());
    REQUIRE(format_wall_clock_hint(500.0, WallClockTier::Normal, "ctest").empty());
}

TEST_CASE("format_wall_clock_hint: Warn 档要求收尾并跑验证", "[wall_clock][issue166]") {
    const std::string hint = format_wall_clock_hint(90.0, WallClockTier::Warn, "ctest -LE slow");
    REQUIRE(hint.find("墙钟") != std::string::npos);
    REQUIRE(hint.find("1m30s") != std::string::npos);           // format_duration 复用
    REQUIRE(hint.find("ctest -LE slow") != std::string::npos);  // 带上项目验证命令
    REQUIRE(hint.find("最终答复") != std::string::npos);

    // 无验证命令时退化为泛化要求，但仍须给出"跑验证"的指令
    const std::string generic = format_wall_clock_hint(90.0, WallClockTier::Warn, "");
    REQUIRE(generic.find("构建/测试") != std::string::npos);
}

TEST_CASE("format_wall_clock_hint: Critical 档要求立即交卷且不提验证命令",
          "[wall_clock][issue166]") {
    // 刻意忽略 verify_command —— 那一刻要的是交卷，不是再跑一遍测试
    const std::string hint = format_wall_clock_hint(8.0, WallClockTier::Critical, "ctest -LE slow");
    REQUIRE(hint.find("墙钟") != std::string::npos);
    REQUIRE(hint.find("8s") != std::string::npos);
    REQUIRE(hint.find("立即停止") != std::string::npos);
    REQUIRE(hint.find("ctest -LE slow") == std::string::npos);

    // 剩余为负（已越过 deadline）按 0s 处理，不产生 "-" 字样
    const std::string overdue = format_wall_clock_hint(-5.0, WallClockTier::Critical, "");
    REQUIRE(overdue.find("0s") != std::string::npos);
    REQUIRE(overdue.find("-") == std::string::npos);
}
