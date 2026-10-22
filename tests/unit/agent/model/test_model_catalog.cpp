/**
 * @file test_model_catalog.cpp
 * @brief models.dev 模型目录测试
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "agent/model/model_catalog.h"

using namespace agent;

namespace {

const char* kSampleApiJson = R"({
  "deepseek": {
    "name": "DeepSeek",
    "models": {
      "deepseek-v4-flash": {
        "limit": { "context": 1048576, "output": 384000 }
      },
      "deepseek-chat": {
        "limit": { "context": 64000, "output": 8192 }
      }
    }
  },
  "moonshot": {
    "name": "Moonshot AI",
    "models": {
      "kimi/kimi-k3": {
        "limit": { "context": 1048576, "output": 128000 }
      }
    }
  },
  "no-models-provider": {
    "name": "Empty",
    "models": {}
  }
})";

}  // anonymous namespace

TEST_CASE("ModelCatalog from_api_json", "[model][catalog]") {
    auto result = ModelCatalog::from_api_json(kSampleApiJson);
    REQUIRE(result.is_ok());

    const auto& catalog = result.value();
    REQUIRE(catalog.size() == 3);

    SECTION("exact match") {
        REQUIRE(catalog.context_window_for("deepseek-v4-flash") == 1'048'576);
        REQUIRE(catalog.max_output_tokens_for("deepseek-v4-flash") == 384'000);
        REQUIRE(catalog.context_window_for("deepseek-chat") == 64'000);
    }

    SECTION("case insensitive") {
        REQUIRE(catalog.context_window_for("DEEPSEEK-V4-FLASH") == 1'048'576);
    }

    SECTION("provider-prefixed model id matches tail") {
        // "kimi/kimi-k3" 索引为 "kimi/kimi-k3"，查询 "kimi-k3" 去前缀命中
        REQUIRE(catalog.context_window_for("kimi-k3") == 1'048'576);
        REQUIRE(catalog.context_window_for("kimi/kimi-k3") == 1'048'576);
    }

    SECTION("longest substring match") {
        REQUIRE(catalog.context_window_for("deepseek-v4-flash-20260424") == 1'048'576);
    }

    SECTION("unknown model returns 0") {
        REQUIRE(catalog.context_window_for("unknown-model") == 0);
    }

    SECTION("contains") {
        REQUIRE(catalog.contains("deepseek-chat"));
        REQUIRE_FALSE(catalog.contains("nonexistent"));
    }

    SECTION("empty provider ignored") { REQUIRE(catalog.size() == 3); }
}

TEST_CASE("ModelCatalog invalid input", "[model][catalog]") {
    SECTION("invalid JSON") {
        auto result = ModelCatalog::from_api_json("not-json{");
        REQUIRE(result.is_err());
    }

    SECTION("non-object root") {
        auto result = ModelCatalog::from_api_json("[1,2,3]");
        REQUIRE(result.is_err());
    }
}

TEST_CASE("ModelCatalog cache roundtrip", "[model][catalog]") {
    auto parsed = ModelCatalog::from_api_json(kSampleApiJson);
    REQUIRE(parsed.is_ok());

    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "workx_test_models_cache.json";
    std::error_code ec;
    fs::remove(tmp, ec);

    auto save = parsed.value().save_cache(tmp);
    REQUIRE(save.is_ok());

    auto loaded = ModelCatalog::load_cache(tmp);
    REQUIRE(loaded.is_ok());
    REQUIRE(loaded.value().size() == 3);
    REQUIRE(loaded.value().context_window_for("deepseek-v4-flash") == 1'048'576);
    REQUIRE(loaded.value().context_window_for("kimi-k3") == 1'048'576);

    fs::remove(tmp, ec);
}

TEST_CASE("ModelCatalog load missing cache", "[model][catalog]") {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "workx_test_no_such_cache.json";
    std::error_code ec;
    fs::remove(tmp, ec);

    auto loaded = ModelCatalog::load_cache(tmp);
    REQUIRE(loaded.is_err());
    REQUIRE(loaded.error().code == Error::Code::ResourceNotFound);
}
