/**
 * @file test_config_manager.cpp
 * @brief ConfigManager 单元测试（V2-1：ResultV2 迁移版）
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <filesystem>
#include "core/config/config_manager.h"
#include "helpers/mock_config_manager.h"  // H-B：ConfigScope DI 测试使用

using namespace agent;
using namespace agent::test;  // H-B：MockConfigManager

TEST_CASE("ConfigManager basic set/get", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    SECTION("set and get int") {
        auto result = cfg.set("test.int_val", 42);
        REQUIRE(result.is_ok());
        auto val = cfg.get<int>("test.int_val");
        REQUIRE(val.is_ok());
        REQUIRE(val.value() == 42);
    }

    SECTION("set and get string") {
        cfg.set("test.str_val", std::string("hello"));
        auto val = cfg.get<std::string>("test.str_val");
        REQUIRE(val.is_ok());
        REQUIRE(val.value() == "hello");
    }

    SECTION("set and get bool") {
        cfg.set("test.bool_val", true);
        auto val = cfg.get<bool>("test.bool_val");
        REQUIRE(val.is_ok());
        REQUIRE(val.value() == true);
    }

    SECTION("set and get double") {
        cfg.set("test.double_val", 3.14);
        auto val = cfg.get<double>("test.double_val");
        REQUIRE(val.is_ok());
        REQUIRE_THAT(val.value(), Catch::Matchers::WithinAbs(3.14, 0.001));
    }

    cfg.clear();
}

TEST_CASE("ConfigManager get_or", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    SECTION("existing key returns value") {
        cfg.set("test.exists", 99);
        REQUIRE(cfg.get_or<int>("test.exists", 0) == 99);
    }

    SECTION("missing key returns default") { REQUIRE(cfg.get_or<int>("test.missing", 42) == 42); }

    cfg.clear();
}

TEST_CASE("ConfigManager validation", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    cfg.register_meta("test.validated",
                      ConfigMeta{.description = "Test validated key",
                                 .default_value = 10,
                                 .is_required = false,
                                 .validate_callback = [](const ConfigValue& v) -> ResultV2<void> {
                                     if (std::holds_alternative<int>(v) && std::get<int>(v) < 0) {
                                         return ResultV2<void>::err(Error::Code::ConfigInvalid,
                                                                    "must be >= 0");
                                     }
                                     return ResultV2<void>::ok();
                                 },
                                 .change_callback = {}});

    SECTION("valid value accepted") {
        auto result = cfg.set("test.validated", 5);
        REQUIRE(result.is_ok());
    }

    SECTION("invalid value rejected with ConfigInvalid") {
        auto result = cfg.set("test.validated", -1);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
    }

    cfg.clear();
}

TEST_CASE("ConfigManager has and remove", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    cfg.set("test.temp", 1);
    REQUIRE(cfg.has("test.temp"));

    cfg.remove("test.temp");
    REQUIRE_FALSE(cfg.has("test.temp"));

    cfg.clear();
}

TEST_CASE("ConfigManager change callback", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    std::string changed_key;
    int old_val = 0;
    int new_val = 0;

    cfg.add_change_callback(
        [&](const std::string& key, const ConfigValue& old_v, const ConfigValue& new_v) {
            changed_key = key;
            if (std::holds_alternative<int>(old_v)) old_val = std::get<int>(old_v);
            if (std::holds_alternative<int>(new_v)) new_val = std::get<int>(new_v);
        });

    cfg.set("test.cb", 10);
    cfg.set("test.cb", 20);

    REQUIRE(changed_key == "test.cb");
    REQUIRE(old_val == 10);
    REQUIRE(new_val == 20);

    cfg.clear_change_callbacks();
    cfg.clear();
}

TEST_CASE("ConfigScope", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    // H-A：ConfigScope 仅依赖 IConfigReader + IConfigWriter（M-4 ISP）
    // ConfigManager 同时实现三者，传同一对象即可
    ConfigScope scope("myapp", cfg, cfg);
    scope.set("width", 800);

    REQUIRE(cfg.has("myapp.width"));
    REQUIRE(scope.get_or<int>("width", 0) == 800);
    REQUIRE(cfg.get_or<int>("myapp.width", 0) == 800);

    cfg.clear();
}

// C-3：ConfigScope DI 化测试（H-4：移除默认实参，所有调用方需显式注入）
// H-B：改用 MockConfigManager 验证 DI 解耦（旧实现注入单例，测试无效）
TEST_CASE("ConfigScope DI injection", "[config][di]") {
    SECTION("注入 MockConfigManager 写入只进 mock，不进单例") {
        // H-B：使用 MockConfigManager 替代 ConfigManager::instance()
        // H-A：ConfigScope 仅依赖 IConfigReader + IConfigWriter，MockConfigManager 实现两者即可
        // 若 ConfigScope 内部偷偷调单例，本测试会失败
        MockConfigManager mock;
        ConfigScope scope("default", mock, mock);

        scope.set("key", 42);

        // 写入应只进 mock，不污染单例
        REQUIRE(mock.has("default.key"));
        REQUIRE(mock.get_or<int>("default.key", 0) == 42);
        REQUIRE(scope.get_or<int>("key", 0) == 42);
        REQUIRE(&scope.config_reader() == &mock);
        REQUIRE(&scope.config_writer() == &mock);

        // 验证全局单例未被污染（DI 解耦的核心证据）
        REQUIRE_FALSE(ConfigManager::instance().has("default.key"));
    }

    SECTION("不同前缀的 scope 写入相互隔离") {
        MockConfigManager mock_a;
        MockConfigManager mock_b;

        ConfigScope scope_a("app.a", mock_a, mock_a);
        ConfigScope scope_b("app.b", mock_b, mock_b);

        scope_a.set("x", 1);
        scope_b.set("x", 2);

        REQUIRE(mock_a.get_or<int>("app.a.x", 0) == 1);
        REQUIRE(mock_b.get_or<int>("app.b.x", 0) == 2);
        REQUIRE_FALSE(mock_a.has("app.b.x"));
        REQUIRE_FALSE(mock_b.has("app.a.x"));
    }
}

// C-2：ConfigSchema 测试
TEST_CASE("ConfigSchema validation", "[config][schema]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    SECTION("Int 范围校验") {
        cfg.register_schema(ConfigSchema{.key = "schema.int_val",
                                         .description = "Test int with range",
                                         .default_value = 50,
                                         .type = ConfigSchema::Type::Int,
                                         .int_range = std::make_pair<int64_t, int64_t>(0, 100)});

        REQUIRE(cfg.set("schema.int_val", 50).is_ok());
        REQUIRE(cfg.set("schema.int_val", 150).is_err());  // 超范围
        REQUIRE(cfg.set("schema.int_val", -1).is_err());   // 超范围
        REQUIRE(cfg.set("schema.int_val", 0).is_ok());     // 边界
        REQUIRE(cfg.set("schema.int_val", 100).is_ok());   // 边界
    }

    SECTION("Enum 校验") {
        cfg.register_schema(ConfigSchema{.key = "schema.enum_val",
                                         .description = "Test enum",
                                         .default_value = std::string("a"),
                                         .type = ConfigSchema::Type::Enum,
                                         .enum_values = {"a", "b", "c"}});

        REQUIRE(cfg.set("schema.enum_val", std::string("a")).is_ok());
        REQUIRE(cfg.set("schema.enum_val", std::string("b")).is_ok());
        REQUIRE(cfg.set("schema.enum_val", std::string("d")).is_err());  // 非法值
    }

    SECTION("类型校验") {
        cfg.register_schema(ConfigSchema{.key = "schema.bool_val",
                                         .description = "Test bool",
                                         .default_value = false,
                                         .type = ConfigSchema::Type::Bool});

        REQUIRE(cfg.set("schema.bool_val", true).is_ok());
        REQUIRE(cfg.set("schema.bool_val", 42).is_err());  // 类型不匹配
    }

    SECTION("get_schema / get_all_schemas") {
        cfg.register_schema(ConfigSchema{.key = "schema.lookup",
                                         .description = "Lookup test",
                                         .default_value = std::string("x"),
                                         .type = ConfigSchema::Type::String});

        auto result = cfg.get_schema("schema.lookup");
        REQUIRE(result.is_ok());
        REQUIRE(result.value().key == "schema.lookup");

        auto all = cfg.get_all_schemas();
        REQUIRE_FALSE(all.empty());
    }

    cfg.clear();
}

TEST_CASE("ConfigManager JSON array save/load roundtrip", "[config]") {
    // backend.providers 多供应商列表（JSON 数组）经 set_nested_json/flatten_json
    // 持久化：数组整体作为 ConfigValue（nlohmann::json）存储，必须实测 save→load 往返
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    auto test_path = std::filesystem::temp_directory_path() / "workx_cfg_roundtrip_test.json";
    nlohmann::json providers = nlohmann::json::array();
    providers.push_back({{"id", "deepseek"},
                         {"name", "DeepSeek"},
                         {"base_url", "https://api.deepseek.com"},
                         {"model", "deepseek-v4-flash"},
                         {"context_length", 1000000},
                         {"api_key", "sk-test"}});
    providers.push_back({{"id", "openai-compatible"},
                         {"name", "Custom URL"},
                         {"base_url", "https://example.com/v1"},
                         {"model", "my-model"},
                         {"context_length", 0},
                         {"api_key", ""}});
    cfg.set("backend.providers", providers);
    cfg.set("backend.provider", std::string("deepseek"));

    auto save_result = cfg.save_to_file(test_path);
    REQUIRE(save_result.is_ok());

    cfg.clear();
    auto load_result = cfg.load_from_file(test_path);
    REQUIRE(load_result.is_ok());

    auto loaded = cfg.get<nlohmann::json>("backend.providers");
    REQUIRE(loaded.is_ok());
    const auto& j = loaded.value();
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 2);
    REQUIRE(j[0]["id"] == "deepseek");
    REQUIRE(j[0]["model"] == "deepseek-v4-flash");
    REQUIRE(j[0]["context_length"] == 1000000);
    REQUIRE(j[1]["id"] == "openai-compatible");
    REQUIRE(j[1]["api_key"] == "");
    REQUIRE(cfg.get_or<std::string>("backend.provider", "") == "deepseek");

    std::filesystem::remove(test_path);
    cfg.clear();
}

// C-4：环境变量加载测试
TEST_CASE("ConfigSchema load_from_env", "[config][env]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    SECTION("环境变量自动加载") {
        cfg.register_schema(ConfigSchema{.key = "env.test_str",
                                         .description = "Env string",
                                         .default_value = std::string("default"),
                                         .type = ConfigSchema::Type::String,
                                         .env_var = "WORKX_TEST_ENV_STR"});

// 设置环境变量
#ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_STR", "from_env");
#else
        setenv("WORKX_TEST_ENV_STR", "from_env", 1);
#endif

        cfg.load_from_env();
        REQUIRE(cfg.get_or<std::string>("env.test_str", "") == "from_env");

// 清理环境变量
#ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_STR", "");
#else
        unsetenv("WORKX_TEST_ENV_STR");
#endif
    }

    SECTION("Int 类型环境变量") {
        cfg.register_schema(ConfigSchema{.key = "env.test_int",
                                         .description = "Env int",
                                         .default_value = 0,
                                         .type = ConfigSchema::Type::Int,
                                         .int_range = std::make_pair<int64_t, int64_t>(0, 1000),
                                         .env_var = "WORKX_TEST_ENV_INT"});

#ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_INT", "42");
#else
        setenv("WORKX_TEST_ENV_INT", "42", 1);
#endif

        cfg.load_from_env();
        REQUIRE(cfg.get_or<int>("env.test_int", 0) == 42);

#ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_INT", "");
#else
        unsetenv("WORKX_TEST_ENV_INT");
#endif
    }

    cfg.clear();
}

// ============================================================
// V2-1 新增：Error::Code 断言测试
// ============================================================

TEST_CASE("ConfigManager V2-1 Error::Code", "[config][v2]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear();

    SECTION("get 缺失键返回 ConfigMissing") {
        auto result = cfg.get<int>("nonexistent.key");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigMissing);
        REQUIRE(result.error().context == "nonexistent.key");
    }

    SECTION("get 类型不匹配返回 ConfigInvalid") {
        cfg.set("type.mismatch", std::string("hello"));
        auto result = cfg.get<int>("type.mismatch");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
        REQUIRE(result.error().context == "type.mismatch");
    }

    SECTION("set Schema 范围校验失败返回 ConfigInvalid") {
        cfg.register_schema(ConfigSchema{.key = "v2.range",
                                         .description = "Range test",
                                         .default_value = 50,
                                         .type = ConfigSchema::Type::Int,
                                         .int_range = std::make_pair<int64_t, int64_t>(0, 100)});

        auto result = cfg.set("v2.range", 200);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
        REQUIRE(result.error().context == "v2.range");
    }

    SECTION("set Schema 枚举校验失败返回 ConfigInvalid") {
        cfg.register_schema(ConfigSchema{.key = "v2.enum",
                                         .description = "Enum test",
                                         .default_value = std::string("a"),
                                         .type = ConfigSchema::Type::Enum,
                                         .enum_values = {"a", "b", "c"}});

        auto result = cfg.set("v2.enum", std::string("z"));
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
        REQUIRE(result.error().context == "v2.enum");
    }

    SECTION("get_schema 缺失返回 ConfigMissing") {
        auto result = cfg.get_schema("nonexistent.schema");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigMissing);
        REQUIRE(result.error().context == "nonexistent.schema");
    }

    SECTION("get_meta 缺失返回 ConfigMissing") {
        auto result = cfg.get_meta("nonexistent.meta");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigMissing);
        REQUIRE(result.error().context == "nonexistent.meta");
    }

    SECTION("load_from_file 文件不存在返回 ResourceNotFound") {
        auto result = cfg.load_from_file("nonexistent_config_file.json");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ResourceNotFound);
    }

    cfg.clear();
}
