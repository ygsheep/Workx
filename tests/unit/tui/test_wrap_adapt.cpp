/**
 * @file test_wrap_adapt.cpp
 * @brief 验证 build_markdown / build_message 折行是否随宽度自适应，
 *        且渲染需求宽度不超过可用宽度（避免文本被右侧截断）。
 */

#include <catch2/catch_test_macros.hpp>

#include <iostream>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

#include "render/markdown_to_elements.h"
#include "render/text_wrap.h"
#include "vm/message_node.h"

using namespace ftxtui;

namespace {

int md_height(std::string_view text, int width) {
    auto node = build_markdown(text, width);
    node->ComputeRequirement();
    return node->requirement().min_y;
}

/// @brief build_markdown 渲染后的最小需求宽度（列）
int md_min_width(std::string_view text, int width) {
    auto node = build_markdown(text, width);
    node->ComputeRequirement();
    return node->requirement().min_x;
}

/// @brief build_message 渲染后的最小需求宽度（列）
int msg_min_width(const MessageNode& m, int width) {
    auto node = build_message(m, width);
    node->ComputeRequirement();
    return node->requirement().min_x;
}

MessageNode assistant_msg(std::string text) {
    MessageNode m;
    m.role = MsgRole::Assistant;
    m.text = std::move(text);
    m.sealed = true;
    return m;
}

}  // namespace

TEST_CASE("wrap adapts to width", "[wrap][adapt]") {
    const std::string long_para =
        "这是一个很长的中文段落用来测试折行是否随宽度自适应变化，"
        "当宽度变窄时应该产生更多行，宽度变宽时行数应该减少。"
        "This is a long English paragraph to test whether wrapping adapts to width, "
        "narrower width should produce more lines and wider width fewer lines.";
    const int h30 = md_height(long_para, 30);
    const int h60 = md_height(long_para, 60);
    const int h120 = md_height(long_para, 120);
    REQUIRE(h30 > h60);
    REQUIRE(h60 > h120);
    REQUIRE(h120 >= 1);
}

TEST_CASE("build_markdown min width never exceeds wrap width", "[wrap][adapt]") {
    // 折行后的每一物理行都不应超过 wrap_w → 需求宽度 ≤ wrap_w
    const std::string long_para =
        "这是一个很长的中文段落用来测试折行是否随宽度自适应变化，"
        "当宽度变窄时应该产生更多行，宽度变宽时行数应该减少。"
        "This is a long English paragraph to test whether wrapping adapts to width, "
        "narrower width should produce more lines and wider width fewer lines.";
    for (const int w : {20, 30, 40, 60, 80, 120}) {
        const int mw = md_min_width(long_para, w);
        if (mw > w) {
            std::cerr << "== width=" << w << " min_width=" << mw << "\n";
            for (auto [b, e] : wrap_text(long_para, w)) {
                const auto seg = long_para.substr(b, e - b);
                std::cerr << "  seg[" << b << "," << e << ") disp=" << utf8_display_width(seg)
                          << " ftxui=" << ftxui::string_width(seg) << " : " << seg << "\n";
            }
        }
        REQUIRE(mw <= w);
    }
}

TEST_CASE("build_message min width fits available space minus indent", "[wrap][adapt]") {
    // build_transcript 中每条消息外层有 text("  ") 2 列缩进；
    // build_message 内正文行又有 text("  ") 2 列缩进。
    // 因此 build_message 的需求宽度必须 ≤ width - 2，否则正文会被右侧截断。
    const std::string long_para =
        "这是一个很长的中文段落用来测试折行是否随宽度自适应变化，"
        "当宽度变窄时应该产生更多行，宽度变宽时行数应该减少。"
        "This is a long English paragraph to test whether wrapping adapts to width, "
        "narrower width should produce more lines and wider width fewer lines.";
    for (const int w : {30, 40, 60, 80, 120}) {
        const int mw = msg_min_width(assistant_msg(long_para), w);
        REQUIRE(mw <= w - 2);
    }
}

TEST_CASE("table cells wrap to fit available width", "[wrap][table]") {
    // 长单元格内容必须按列宽折行：渲染需求宽度 ≤ 折行宽度，
    // 且宽度变窄时行数增加（单元格折行生效）。
    const std::string md =
        "| 名称 | 描述 |\n"
        "| --- | --- |\n"
        "| 功能 | "
        "这是一个非常非常非常长的描述文本用来测试表格单元格内容是否能够根据列宽自动换行不溢出显示区"
        "域 |\n"
        "| 限制 | O(n log n) 平均复杂度，最坏情况 O(n^2) |";
    for (const int w : {30, 40, 60, 80}) {
        const int mw = md_min_width(md, w);
        REQUIRE(mw <= w);
    }
    // 窄宽度下长单元格折成多行 → 高度显著大于宽宽度
    const int h30 = md_height(md, 30);
    const int h80 = md_height(md, 80);
    REQUIRE(h30 > h80);
}

TEST_CASE("table inside message fits available space", "[wrap][table]") {
    // 表格位于助手消息正文：build_message 需求宽度 ≤ width - 2（外层缩进）
    const std::string md =
        "| 场景 | 复杂度 |\n"
        "| --- | --- |\n"
        "| 平均 | O(n log n) |\n"
        "| 最坏 | O(n^2) |";
    for (const int w : {30, 40, 60, 80, 120}) {
        const int mw = msg_min_width(assistant_msg(md), w);
        REQUIRE(mw <= w - 2);
    }
}

TEST_CASE("wrapped table estimate matches render at narrow widths", "[wrap][table][layout]") {
    // 折行后行高估算必须与渲染一致（A3 单一布局源），否则滚动定位漂移
    const std::string md =
        "| 名称 | 描述 |\n"
        "| --- | --- |\n"
        "| 功能 | "
        "这是一个非常非常非常长的描述文本用来测试表格单元格内容是否能够根据列宽自动换行不溢出显示区"
        "域 |\n"
        "| 限制 | O(n log n) 平均复杂度，最坏情况 O(n^2) |";
    for (const int w : {24, 30, 40, 60, 80}) {
        const int est = estimate_markdown_height(md, w);
        const int rnd = md_height(md, w);
        REQUIRE(est == rnd);
    }
}
