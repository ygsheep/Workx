/**
 * @file test_env_setup_hint.cpp
 * @brief 环境自建提示单元测试（Issue #176）
 * @details `format_env_setup_hint` 是纯函数：只吃一份「常用工具探测结果」，
 *          不读配置、不碰时钟、不起子进程，因此结果完全确定，可直接断言。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "agent/prompt/env_setup_hint.h"

namespace {

using agent::prompt::format_env_setup_hint;
using agent::prompt::kEnvSetupMaxListed;
using agent::prompt::ToolchainInfo;

ToolchainInfo make(std::string name, bool found) {
    ToolchainInfo t;
    t.name = std::move(name);
    t.found = found;
    return t;
}

}  // namespace

TEST_CASE("环境自建：没有缺失工具时返回空串", "[issue176][env_setup_hint]") {
    const std::vector<ToolchainInfo> tools = {make("git", true), make("curl", true)};
    CHECK(format_env_setup_hint(tools).empty());
}

TEST_CASE("环境自建：空输入返回空串", "[issue176][env_setup_hint]") {
    CHECK(format_env_setup_hint({}).empty());
}

TEST_CASE("环境自建：列出缺失工具并给出后台安装建议", "[issue176][env_setup_hint]") {
    const std::vector<ToolchainInfo> tools = {make("git", true), make("ffmpeg", false),
                                              make("tesseract", false)};

    const std::string text = format_env_setup_hint(tools);

    CHECK_FALSE(text.empty());
    CHECK(text.find("## Environment setup") != std::string::npos);
    // 缺失的列出来、已装的不要出现
    CHECK(text.find("`ffmpeg`") != std::string::npos);
    CHECK(text.find("`tesseract`") != std::string::npos);
    CHECK(text.find("`git`") == std::string::npos);
    // 关键策略：后台执行 + 批量非交互安装
    CHECK(text.find("run_in_background=true") != std::string::npos);
    CHECK(text.find("DEBIAN_FRONTEND=noninteractive") != std::string::npos);
}

TEST_CASE("环境自建：缺失过多时截断并提示余量", "[issue176][env_setup_hint]") {
    std::vector<ToolchainInfo> tools;
    const std::size_t total = kEnvSetupMaxListed + 3;
    for (std::size_t i = 0; i < total; ++i) {
        tools.push_back(make("tool" + std::to_string(i), false));
    }

    const std::string text = format_env_setup_hint(tools);

    CHECK_FALSE(text.empty());
    CHECK(text.find("`tool0`") != std::string::npos);
    // 第 kEnvSetupMaxListed 个之后不应再逐个列出
    CHECK(text.find("`tool" + std::to_string(kEnvSetupMaxListed) + "`") == std::string::npos);
    CHECK(text.find("另有 3 个") != std::string::npos);
}
