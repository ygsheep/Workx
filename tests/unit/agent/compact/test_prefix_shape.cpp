/**
 * @file test_prefix_shape.cpp
 * @brief 前缀形状追踪与缓存诊断单元测试（DS_CACHE H-1）
 */

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "agent/compact/prefix_shape.h"

using namespace agent;
using json = nlohmann::json;

// ============================================================
// capture_shape — 形状捕获
// ============================================================

TEST_CASE("capture_shape basic", "[compact][prefix_shape][ds_cache]") {
    SECTION("empty inputs produce non-empty hashes") {
        PrefixShape shape = capture_shape("", json::array(), 0);
        REQUIRE_FALSE(shape.system_hash.empty());
        REQUIRE_FALSE(shape.tools_hash.empty());
        REQUIRE(shape.prefix_hash == shape.system_hash + "|" + shape.tools_hash);
        REQUIRE(shape.rewrite_version == 0);
    }

    SECTION("different system prompts produce different hashes") {
        PrefixShape s1 = capture_shape("prompt A", json::array(), 0);
        PrefixShape s2 = capture_shape("prompt B", json::array(), 0);
        REQUIRE(s1.system_hash != s2.system_hash);
        REQUIRE(s1.prefix_hash != s2.prefix_hash);
    }

    SECTION("same inputs produce same hash (deterministic)") {
        json tools = {
            {{"type", "function"}, {"function", {{"name", "Read"}, {"description", "d"}}}},
            {{"type", "function"}, {"function", {{"name", "Bash"}, {"description", "d"}}}}};
        PrefixShape s1 = capture_shape("sys", tools, 0);
        PrefixShape s2 = capture_shape("sys", tools, 0);
        REQUIRE(s1.prefix_hash == s2.prefix_hash);
    }

    SECTION("rewrite_version stored in shape") {
        PrefixShape s = capture_shape("sys", json::array(), 7);
        REQUIRE(s.rewrite_version == 7);
    }
}

// ============================================================
// normalize_tools_schema — 排序确定性
// ============================================================

TEST_CASE("normalize_tools_schema sort determinism", "[compact][prefix_shape][ds_cache]") {
    SECTION("sorted by function.name") {
        json tools_unsorted = {
            {{"type", "function"}, {"function", {{"name", "Zebra"}, {"description", "d"}}}},
            {{"type", "function"}, {"function", {{"name", "Alpha"}, {"description", "d"}}}},
            {{"type", "function"}, {"function", {{"name", "Middle"}, {"description", "d"}}}}};
        json normalized = normalize_tools_schema(tools_unsorted);
        REQUIRE(normalized.size() == 3);
        REQUIRE(normalized[0]["function"]["name"] == "Alpha");
        REQUIRE(normalized[1]["function"]["name"] == "Middle");
        REQUIRE(normalized[2]["function"]["name"] == "Zebra");
    }

    SECTION("different registration order produces same normalized result") {
        json order_a = {
            {{"type", "function"}, {"function", {{"name", "Read"}, {"description", "d"}}}},
            {{"type", "function"}, {"function", {{"name", "Bash"}, {"description", "d"}}}}};
        json order_b = {
            {{"type", "function"}, {"function", {{"name", "Bash"}, {"description", "d"}}}},
            {{"type", "function"}, {"function", {{"name", "Read"}, {"description", "d"}}}}};
        auto norm_a = normalize_tools_schema(order_a);
        auto norm_b = normalize_tools_schema(order_b);
        REQUIRE(norm_a.dump() == norm_b.dump());
    }

    SECTION("order-independent hash via capture_shape") {
        json order_a = {{{"type", "function"}, {"function", {{"name", "Read"}}}},
                        {{"type", "function"}, {"function", {{"name", "Bash"}}}}};
        json order_b = {{{"type", "function"}, {"function", {{"name", "Bash"}}}},
                        {{"type", "function"}, {"function", {{"name", "Read"}}}}};
        PrefixShape s_a = capture_shape("sys", order_a, 0);
        PrefixShape s_b = capture_shape("sys", order_b, 0);
        REQUIRE(s_a.tools_hash == s_b.tools_hash);
        REQUIRE(s_a.prefix_hash == s_b.prefix_hash);
    }

    SECTION("empty/null tools produce empty array") {
        REQUIRE(normalize_tools_schema(json::array()).empty());
        REQUIRE(normalize_tools_schema(json(nullptr)).empty());
    }

    SECTION("direct name field (non-OpenAI format)") {
        json tools = {{{"name", "Zebra"}}, {{"name", "Alpha"}}};
        auto normalized = normalize_tools_schema(tools);
        REQUIRE(normalized[0]["name"] == "Alpha");
        REQUIRE(normalized[1]["name"] == "Zebra");
    }
}

// ============================================================
// compare_shape — 三类归因
// ============================================================

TEST_CASE("compare_shape attribution", "[compact][prefix_shape][ds_cache]") {
    SECTION("no change: empty reasons") {
        PrefixShape prev = capture_shape("sys", json::array(), 0);
        PrefixShape cur = capture_shape("sys", json::array(), 0);
        auto diag = compare_shape(prev, cur, 100, 10);
        REQUIRE_FALSE(diag.prefix_changed);
        REQUIRE(diag.reasons.empty());
    }

    SECTION("system change: reason='system'") {
        PrefixShape prev = capture_shape("sys A", json::array(), 0);
        PrefixShape cur = capture_shape("sys B", json::array(), 0);
        auto diag = compare_shape(prev, cur, 50, 200);
        REQUIRE(diag.prefix_changed);
        REQUIRE(diag.reasons.size() == 1);
        REQUIRE(diag.reasons[0] == "system");
    }

    SECTION("tools change: reason='tools'") {
        json tools_a = {{{"type", "function"}, {"function", {{"name", "Read"}}}}};
        json tools_b = {{{"type", "function"}, {"function", {{"name", "Bash"}}}}};
        PrefixShape prev = capture_shape("sys", tools_a, 0);
        PrefixShape cur = capture_shape("sys", tools_b, 0);
        auto diag = compare_shape(prev, cur, 50, 200);
        REQUIRE(diag.prefix_changed);
        REQUIRE(diag.reasons.size() == 1);
        REQUIRE(diag.reasons[0] == "tools");
    }

    SECTION("log_rewrite: rewrite_version change") {
        PrefixShape prev = capture_shape("sys", json::array(), 0);
        PrefixShape cur = capture_shape("sys", json::array(), 3);
        auto diag = compare_shape(prev, cur, 50, 500);
        REQUIRE(diag.prefix_changed);
        REQUIRE(diag.reasons.size() == 1);
        REQUIRE(diag.reasons[0] == "log_rewrite");
    }

    SECTION("multiple reasons combined") {
        PrefixShape prev =
            capture_shape("sys A", {{{"type", "function"}, {"function", {{"name", "Read"}}}}}, 0);
        PrefixShape cur =
            capture_shape("sys B", {{{"type", "function"}, {"function", {{"name", "Bash"}}}}}, 5);
        auto diag = compare_shape(prev, cur, 0, 1000);
        REQUIRE(diag.prefix_changed);
        REQUIRE(diag.reasons.size() == 3);
        // reasons 顺序：system, tools, log_rewrite
        REQUIRE(diag.reasons[0] == "system");
        REQUIRE(diag.reasons[1] == "tools");
        REQUIRE(diag.reasons[2] == "log_rewrite");
    }

    SECTION("first turn: prev empty, not diagnosed as changed") {
        PrefixShape prev;  // 默认构造，prefix_hash 为空
        PrefixShape cur = capture_shape("sys", json::array(), 0);
        auto diag = compare_shape(prev, cur, 0, 1000);
        REQUIRE_FALSE(diag.prefix_changed);
        REQUIRE(diag.reasons.empty());
    }

    SECTION("hit/miss tokens transparent passthrough") {
        PrefixShape prev = capture_shape("sys", json::array(), 0);
        PrefixShape cur = capture_shape("sys", json::array(), 0);
        auto diag = compare_shape(prev, cur, 800, 200);
        REQUIRE(diag.cache_hit_tokens == 800);
        REQUIRE(diag.cache_miss_tokens == 200);
    }
}
