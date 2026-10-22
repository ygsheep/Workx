/**
 * @file test_context_resolver.cpp
 * @brief 上下文窗口解析器测试（含 ModelCatalog 优先级）
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "agent/model/context_resolver.h"
#include "agent/model/config.h"
#include "agent/model/model_catalog.h"
#include "agent/model/provider_preset.h"

using namespace agent;

namespace {

/// @brief 构造含指定模型的测试目录（shared_ptr：与 resolver 新签名一致）
std::shared_ptr<const ModelCatalog> make_catalog(std::string_view json) {
    auto result = ModelCatalog::from_api_json(json);
    REQUIRE(result.is_ok());
    return std::make_shared<const ModelCatalog>(std::move(result.value()));
}

}  // anonymous namespace

TEST_CASE("resolve_context_length priority chain", "[model][resolver]") {
    auto catalog = make_catalog(R"({
        "test": { "models": {
            "catalog-only-model": { "limit": { "context": 999000, "output": 1000 } }
        } }
    })");

    SECTION("1. provider list_models wins") {
        auto r = resolve_context_length("claude-sonnet-4-5", 300'000, 0, nullptr, catalog);
        REQUIRE(r.value == 300'000);
        REQUIRE(r.source == ContextLengthResolution::Source::ProviderList);
    }

    SECTION("2. user config wins over catalog") {
        auto r = resolve_context_length("catalog-only-model", 0, 123'456, nullptr, catalog);
        REQUIRE(r.value == 123'456);
        REQUIRE(r.source == ContextLengthResolution::Source::UserConfig);
    }

    SECTION("3. catalog wins over static table") {
        // deepseek-v4-flash 静态表 1M，catalog 覆盖为 2M
        auto override_catalog = make_catalog(R"({
            "test": { "models": {
                "deepseek-v4-flash": { "limit": { "context": 2000000, "output": 1000 } }
            } }
        })");
        auto r = resolve_context_length("deepseek-v4-flash", 0, 0, nullptr, override_catalog);
        REQUIRE(r.value == 2'000'000);
        REQUIRE(r.source == ContextLengthResolution::Source::ModelCatalog);
    }

    SECTION("4. static table when catalog misses") {
        auto r = resolve_context_length("claude-sonnet-4-5", 0, 0, nullptr, catalog);
        REQUIRE(r.value == 200'000);
        REQUIRE(r.source == ContextLengthResolution::Source::ModelCapability);
    }

    SECTION("5. preset default when both miss") {
        const ProviderPreset* preset = find_preset("deepseek");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->default_context_length > 0);
        auto r = resolve_context_length("deepseek-unknown-model", 0, 0, preset, catalog);
        REQUIRE(r.value == preset->default_context_length);
        REQUIRE(r.source == ContextLengthResolution::Source::PresetDefault);
    }

    SECTION("6. default constant fallback") {
        auto r = resolve_context_length("totally-unknown-model", 0, 0, nullptr, catalog);
        REQUIRE(r.value == MODEL_CONTEXT_WINDOW_DEFAULT);
        REQUIRE(r.source == ContextLengthResolution::Source::Default);
    }
}

TEST_CASE("resolve_context_length catalog parameter", "[model][resolver]") {
    SECTION("nullptr catalog falls back to static table") {
        auto r = resolve_context_length("gpt-4o", 0, 0, nullptr, nullptr);
        REQUIRE(r.value == 128'000);
        REQUIRE(r.source == ContextLengthResolution::Source::ModelCapability);
    }

    SECTION("empty catalog behaves like nullptr") {
        auto empty_catalog = std::make_shared<const ModelCatalog>();
        auto r = resolve_context_length("gpt-4o", 0, 0, nullptr, empty_catalog);
        REQUIRE(r.value == 128'000);
    }
}
