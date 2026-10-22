/**
 * @file test_pricing_table.cpp
 * @brief 单价表单测
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include <string>

#include "island/pricing_table.h"

namespace {
bool close_enough(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

using island::PricingTable;

TEST_CASE("pricing: deepseek default table", "[island][pricing]") {
    const auto table = PricingTable::deepseek_default();
    REQUIRE(table.all().size() >= 3);

    const auto* chat = table.find("deepseek-chat");
    REQUIRE(chat != nullptr);
    REQUIRE(close_enough(chat->input_per_1m, 0.27));
    REQUIRE(close_enough(chat->output_per_1m, 1.10));
    REQUIRE(close_enough(chat->cache_read_per_1m, 0.07));
    REQUIRE(close_enough(chat->cache_write_per_1m, 0.27));

    REQUIRE(table.find("deepseek-v4-flash") != nullptr);
    REQUIRE(table.find("deepseek-v4-reasoner") != nullptr);
}

TEST_CASE("pricing: unknown model returns nullptr", "[island][pricing]") {
    const auto table = PricingTable::deepseek_default();
    REQUIRE(table.find("gpt-4o") == nullptr);
    REQUIRE(table.find("") == nullptr);
}

TEST_CASE("pricing: load from json", "[island][pricing]") {
    const std::string json_text = R"([
        {"model": "custom-model", "input_per_1m": 1.0, "output_per_1m": 2.0,
         "cache_read_per_1m": 0.5, "cache_write_per_1m": 1.5, "context_window": 32000}
    ])";
    const auto result = PricingTable::load_from_json(json_text);
    REQUIRE(result.is_ok());
    const auto* p = result.value().find("custom-model");
    REQUIRE(p != nullptr);
    REQUIRE(close_enough(p->input_per_1m, 1.0));
    REQUIRE(close_enough(p->output_per_1m, 2.0));
    REQUIRE(close_enough(p->cache_read_per_1m, 0.5));
    REQUIRE(close_enough(p->cache_write_per_1m, 1.5));
    REQUIRE(p->context_window == 32000);
}

TEST_CASE("pricing: load_from_json rejects invalid input", "[island][pricing]") {
    REQUIRE(PricingTable::load_from_json("{\"model\":\"x\"}").is_err());
    REQUIRE(PricingTable::load_from_json("[{\"output_per_1m\": 1.0}]").is_err());
    REQUIRE(PricingTable::load_from_json("not-json").is_err());
    REQUIRE(PricingTable::load_from_json("[]").is_err());
}

TEST_CASE("pricing: to_json round trip", "[island][pricing]") {
    auto table = PricingTable::deepseek_default();
    const nlohmann::json j = table.to_json();
    REQUIRE(j.is_array());
    REQUIRE(j.size() == table.all().size());

    const auto reloaded = PricingTable::load_from_json(j.dump());
    REQUIRE(reloaded.is_ok());
    REQUIRE(reloaded.value().all().size() == table.all().size());
}
