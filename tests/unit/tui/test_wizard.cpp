/**
 * @file test_wizard.cpp
 * @brief 首次运行设置向导配置写入无头单元测试
 * @details 覆盖 apply_wizard_config：预设 provider 写入 provider/api_key/
 *          model_name/context_length、自定义 URL 预设写 remote_url、非法
 *          上下文长度忽略、未知 provider 拒绝、落盘往返。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

#include "agent/config/app_config.h"
#include "core/config/config_manager.h"
#include "wizard.h"

using namespace ftxtui;

namespace {
/// @brief 临时配置文件路径（每个用例独立，测试结束自动删除）
struct TempConfig {
    std::filesystem::path path;
    TempConfig() {
        path =
            std::filesystem::temp_directory_path() / ("workx_wizard_" + std::to_string(::rand()) +
                                                      "_" + std::to_string(::clock()) + ".json");
    }
    ~TempConfig() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

/// @brief 单例 ConfigManager 的隔离作用域（clear 后开始，避免用例间状态泄漏）
struct ConfigScope {
    agent::ConfigManager& cfg;
    ConfigScope() : cfg(agent::ConfigManager::instance()) { cfg.clear(); }
    ~ConfigScope() { cfg.clear(); }
};
}  // namespace

TEST_CASE("wizard config writes preset provider and defaults", "[wizard]") {
    TempConfig tf;
    ConfigScope cs;
    WizardConfig wc;
    wc.provider = "deepseek";
    wc.api_key = "sk-test-123";
    wc.context_len = "64000";

    REQUIRE(apply_wizard_config(cs.cfg, tf.path, wc));

    REQUIRE(cs.cfg.get_or<std::string>(agent::keys::PROVIDER, "") == "deepseek");
    REQUIRE(cs.cfg.get_or<std::string>(agent::keys::API_KEY, "") == "sk-test-123");
    REQUIRE_FALSE(cs.cfg.get_or<std::string>(agent::keys::MODEL_NAME, "").empty());
    REQUIRE(cs.cfg.get_or<int>(agent::keys::CONTEXT_LENGTH, 0) == 64000);
    REQUIRE(std::filesystem::exists(tf.path));
}

TEST_CASE("wizard config custom url preset writes remote_url", "[wizard]") {
    TempConfig tf;
    ConfigScope cs;
    WizardConfig wc;
    wc.provider = "openai-compatible";
    wc.api_key = "sk-custom";
    wc.custom_url = "https://example.com/v1/chat/completions";

    REQUIRE(apply_wizard_config(cs.cfg, tf.path, wc));
    REQUIRE(cs.cfg.get_or<std::string>(agent::keys::PROVIDER, "") == "openai-compatible");
    REQUIRE(cs.cfg.get_or<std::string>(agent::keys::REMOTE_URL, "") ==
            "https://example.com/v1/chat/completions");
}

TEST_CASE("wizard config ignores invalid context length", "[wizard]") {
    TempConfig tf;
    ConfigScope cs;
    WizardConfig wc;
    wc.provider = "glm";
    wc.context_len = "not-a-number";

    REQUIRE(apply_wizard_config(cs.cfg, tf.path, wc));
    REQUIRE(cs.cfg.get_or<int>(agent::keys::CONTEXT_LENGTH, 0) == 0);
}

TEST_CASE("wizard config rejects unknown provider", "[wizard]") {
    TempConfig tf;
    ConfigScope cs;
    WizardConfig wc;
    wc.provider = "no-such-provider";

    REQUIRE_FALSE(apply_wizard_config(cs.cfg, tf.path, wc));
}

TEST_CASE("wizard config round-trips through file", "[wizard]") {
    TempConfig tf;
    {
        ConfigScope cs;
        WizardConfig wc;
        wc.provider = "qwen";
        wc.api_key = "sk-qwen";
        wc.context_len = "128000";
        REQUIRE(apply_wizard_config(cs.cfg, tf.path, wc));
    }
    // 重新加载验证落盘内容
    ConfigScope cs;
    const auto result = cs.cfg.load_from_file(tf.path);
    REQUIRE(result.is_ok());
    REQUIRE(cs.cfg.get_or<std::string>(agent::keys::PROVIDER, "") == "qwen");
    REQUIRE(cs.cfg.get_or<std::string>(agent::keys::API_KEY, "") == "sk-qwen");
    REQUIRE(cs.cfg.get_or<int>(agent::keys::CONTEXT_LENGTH, 0) == 128000);
}
