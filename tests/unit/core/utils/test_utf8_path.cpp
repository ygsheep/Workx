/**
 * @file test_utf8_path.cpp
 * @brief 路径 → UTF-8 转换单测（#182：非 UTF-8 路径字节致 json::dump() 抛 type_error.316）
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "core/utils/utf8_path.h"

using core::util::path_to_utf8;

namespace {

/// "表情包" 的 UTF-8 字节（E8 A1 A8 / E6 83 85 / E5 8C 85）
constexpr const char* kChineseUtf8 = "\xE8\xA1\xA8\xE6\x83\x85\xE5\x8C\x85";

}  // namespace

TEST_CASE("path_to_utf8 空路径返回空串", "[core][utils][utf8_path]") {
    REQUIRE(path_to_utf8(std::filesystem::path{}).empty());
}

TEST_CASE("path_to_utf8 单段 ASCII 名原样返回", "[core][utils][utf8_path]") {
    REQUIRE(path_to_utf8(std::filesystem::path("build")) == "build");
}

#ifdef _WIN32
TEST_CASE("path_to_utf8 中文路径给出合法 UTF-8（而非 GBK）", "[core][utils][utf8_path]") {
    // Windows 上 path::string() 返回本机窄编码（中文系统 = GBK），这些字节非法 UTF-8，
    // 进入 JSON 会让 dump() 抛 type_error.316（#182：请求发不出去 / 启动即崩）。
    REQUIRE(path_to_utf8(std::filesystem::path(L"表情包")) == kChineseUtf8);

    // 多段路径：分隔符保持本机原生形式（与改动前的 path::string() 一致，只修编码）
    const std::filesystem::path nested = std::filesystem::path(L"a") / L"表情包" / L"b.txt";
    REQUIRE(path_to_utf8(nested) == std::string("a\\") + kChineseUtf8 + "\\b.txt");
}
#else
TEST_CASE("path_to_utf8 在 POSIX 上与 string() 等价", "[core][utils][utf8_path]") {
    // POSIX 路径本身就是 UTF-8 字节，无需转换
    const std::filesystem::path p = std::filesystem::path(kChineseUtf8);
    REQUIRE(path_to_utf8(p) == p.string());
    REQUIRE(path_to_utf8(p) == kChineseUtf8);
}
#endif
