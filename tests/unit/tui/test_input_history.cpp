/**
 * @file test_input_history.cpp
 * @brief 输入历史持久化无头单元测试
 * @details 覆盖：push 去重/上限、prev/next 导航与草稿还原、load/save 落盘往返、
 *          reset_nav 复位、损坏文件容错。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <string>

#include "widgets/input_history.h"

using namespace ftxtui;

namespace {
/// @brief 临时文件路径（每个用例独立，测试结束自动删除）
struct TempFile {
    std::filesystem::path path;
    TempFile() {
        path =
            std::filesystem::temp_directory_path() / ("workx_history_" + std::to_string(::rand()) +
                                                      "_" + std::to_string(::clock()) + ".json");
    }
    ~TempFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};
}  // namespace

TEST_CASE("history push dedups consecutive and caps size", "[history]") {
    InputHistory h;
    for (int i = 0; i < 250; ++i) h.push("msg" + std::to_string(i));
    REQUIRE(h.size() == 200);  // 上限 200
    h.push("msg199");          // 与最近一条相同 → 跳过
    REQUIRE(h.size() == 200);
    h.push("");  // 空输入不入历史
    REQUIRE(h.size() == 200);
}

TEST_CASE("history prev/next navigates and restores draft", "[history]") {
    InputHistory h;
    h.push("first");
    h.push("second");
    h.push("third");

    std::string out;
    // 首次 prev：记录草稿 "draft"，回到第三条
    REQUIRE(h.prev("draft", out));
    REQUIRE(out == "third");
    REQUIRE(h.prev("draft", out));
    REQUIRE(out == "second");
    REQUIRE(h.prev("draft", out));
    REQUIRE(out == "first");
    REQUIRE_FALSE(h.prev("draft", out));  // 已到最旧

    // 下箭头：逐条前进，最后还原草稿
    REQUIRE(h.next(out));
    REQUIRE(out == "second");
    REQUIRE(h.next(out));
    REQUIRE(out == "third");
    REQUIRE(h.next(out));
    REQUIRE(out == "draft");     // 回到草稿位还原草稿
    REQUIRE_FALSE(h.next(out));  // 已在草稿位
}

TEST_CASE("history prev on empty history returns false", "[history]") {
    InputHistory h;
    std::string out;
    REQUIRE_FALSE(h.prev("draft", out));
    REQUIRE_FALSE(h.next(out));
}

TEST_CASE("history reset_nav returns to draft position", "[history]") {
    InputHistory h;
    h.push("a");
    h.push("b");
    std::string out;
    REQUIRE(h.prev("draft", out));
    REQUIRE(out == "b");
    h.reset_nav();
    REQUIRE_FALSE(h.next(out));  // 复位后已在草稿位
    REQUIRE(h.prev("draft", out));
    REQUIRE(out == "b");
}

TEST_CASE("history save/load round-trips entries", "[history]") {
    TempFile tf;
    {
        InputHistory h;
        h.load(tf.path);  // 设置落盘路径
        h.push("hello world");
        h.push("多行\n输入");
        h.push("/model gpt-4");
        h.save();
    }
    InputHistory h2;
    h2.load(tf.path);
    REQUIRE(h2.size() == 3);
    std::string out;
    REQUIRE(h2.prev("", out));
    REQUIRE(out == "/model gpt-4");
    REQUIRE(h2.prev("", out));
    REQUIRE(out == "多行\n输入");
    REQUIRE(h2.prev("", out));
    REQUIRE(out == "hello world");
}

TEST_CASE("history load tolerates missing and corrupt files", "[history]") {
    InputHistory h;
    h.load(std::filesystem::temp_directory_path() / "workx_history_does_not_exist_12345.json");
    REQUIRE(h.size() == 0);

    TempFile tf;
    {
        std::FILE* f = std::fopen(tf.path.string().c_str(), "wb");
        REQUIRE(f != nullptr);
        std::fputs("{ not valid json", f);
        std::fclose(f);
    }
    InputHistory h2;
    h2.load(tf.path);
    REQUIRE(h2.size() == 0);
}
