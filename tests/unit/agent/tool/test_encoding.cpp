/**
 * @file test_encoding.cpp
 * @brief sanitize_utf8 / sanitize_json_strings 单元测试
 * @details 回归防护：sanitize_utf8 曾被误实现为丢弃 ASCII 字节，
 *          导致纯 ASCII 工具输出（echo/pwd/git/Glob 等）被清空。
 */

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "agent/tool/encoding.h"

using namespace agent::tool;

TEST_CASE("sanitize_utf8 preserves pure ASCII", "[tool][encoding]") {
    const std::string input = "echo hello_world";
    REQUIRE(sanitize_utf8(input) == input);
}

TEST_CASE("sanitize_utf8 preserves valid multibyte", "[tool][encoding]") {
    const std::string input = "版本单一事实源";
    REQUIRE(sanitize_utf8(input) == input);
}

TEST_CASE("sanitize_utf8 keeps mixed ascii+utf8 intact", "[tool][encoding]") {
    const std::string input = "// 版本单一事实源\nx=1;\r\n中文;";
    REQUIRE(sanitize_utf8(input) == input);
}

TEST_CASE("sanitize_utf8 replaces invalid lone continuation byte", "[tool][encoding]") {
    // 0x88 为孤立续字节（GBK 次字节），应替换为 U+FFFD (EF BF BD)
    // 拆分转义：MSVC 将 "\x88b" 贪婪解析为单个 \x88b（越界），需 "\x88" "b"
    const std::string input =
        "a\x88"
        "b";
    const std::string expected =
        "a\xEF\xBF\xBD"
        "b";
    REQUIRE(sanitize_utf8(input) == expected);
}

TEST_CASE("sanitize_utf8 replaces truncated sequence tail", "[tool][encoding]") {
    // 3 字节序列只剩 2 字节（截断），尾字节替换为 U+FFFD
    const std::string input =
        "ab\xE6"
        "\x96";
    const std::string expected = "ab\xEF\xBF\xBD";
    REQUIRE(sanitize_utf8(input) == expected);
}

TEST_CASE("sanitize_json_strings cleans nested strings", "[tool][encoding]") {
    nlohmann::json j = {{"k", std::string("v\x88")},
                        {"arr", nlohmann::json::array({std::string("\xBC")})},
                        {"n", 42}};
    auto cleaned = sanitize_json_strings(j);
    REQUIRE(cleaned["k"] == std::string("v\xEF\xBF\xBD"));
    REQUIRE(cleaned["arr"][0] == std::string("\xEF\xBF\xBD"));
    REQUIRE(cleaned["n"] == 42);
}
