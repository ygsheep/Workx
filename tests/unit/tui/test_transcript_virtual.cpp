/**
 * @file test_transcript_virtual.cpp
 * @brief 转录虚拟化切片逻辑单测（不依赖 UI/FTXUI 布局）
 * @details 验证 select_transcript_slice：可视切片必须覆盖视口（含缓冲）、
 *          top_pad/bottom_pad 与切片内容高度之和等于总内容高度（不改变
 *          滚动范围），并覆盖顶部/中间/底部与空转录边界。
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "render/transcript_layout.h"

using namespace ftxtui;

namespace {

/// @brief 由每条消息高度 h_i(>=1) 构造前缀数组（每条后含 1 行间距）
std::vector<int> make_prefix(const std::vector<int>& heights) {
    std::vector<int> prefix(heights.size() + 1, 0);
    for (std::size_t i = 0; i < heights.size(); ++i) prefix[i + 1] = prefix[i] + heights[i] + 1;
    return prefix;
}

/// @brief 定位包含内容行 row 的消息下标（复制实现，独立校验）
std::size_t containing(const std::vector<int>& prefix, int row) {
    if (row < 0) return 0;
    std::size_t n = prefix.size() - 1;
    auto it = std::upper_bound(prefix.begin(), prefix.end(), row);
    std::size_t idx = static_cast<std::size_t>(it - prefix.begin()) - 1;
    return std::min(idx, n - 1);
}

}  // namespace

TEST_CASE("empty prefix => empty slice") {
    {
        TranscriptSlice s = select_transcript_slice({}, 0, 10, 0);
        REQUIRE(s.empty);
    }
    {
        // 仅「无消息」哨兵：prefix={0}
        TranscriptSlice s = select_transcript_slice({0}, 0, 10, 0);
        REQUIRE(s.empty);
    }
}

TEST_CASE("single message, all scroll tops stay valid") {
    // 单消息 h=3：prefix={0,4}，总高 4
    const auto prefix = make_prefix({3});
    for (int top = 0; top <= 4; ++top) {
        auto s = select_transcript_slice(prefix, top, 3, 0);
        if (top >= 4)
            REQUIRE(s.empty);
        else {
            REQUIRE_FALSE(s.empty);
            REQUIRE(s.first == 0);
            REQUIRE(s.last == 0);
            REQUIRE(s.top_pad == 0);
            REQUIRE(s.bottom_pad == 0);
        }
    }
}

TEST_CASE("slices cover viewport and preserve total height") {
    // 5 条消息，高度各不相同：{1, 2, 4, 8, 3}
    const auto prefix = make_prefix({1, 2, 4, 8, 3});
    const int total = prefix[5];

    for (const int margin : {0, 2, 6}) {
        for (const int avail : {3, 10}) {
            for (int top = 0; top <= total + 5; ++top) {
                auto s = select_transcript_slice(prefix, top, avail, margin);
                int row_lo = std::max(0, top - margin);
                int row_hi = std::min(total - 1, top + avail - 1 + margin);
                if (row_lo >= total) {
                    REQUIRE(s.empty);
                    continue;
                }
                REQUIRE_FALSE(s.empty);

                // 边界正确
                REQUIRE(s.first <= s.last);
                REQUIRE(s.last < prefix.size() - 1);
                REQUIRE(s.first == containing(prefix, row_lo));
                REQUIRE(s.last == containing(prefix, row_hi));

                // 高度不变量：顶留白 + 切片内容 + 底留白 == 总内容高度
                REQUIRE(s.top_pad == prefix[s.first]);
                REQUIRE(s.bottom_pad == prefix[prefix.size() - 1] - prefix[s.last + 1]);
                int content = prefix[s.last + 1] - prefix[s.first];
                REQUIRE(s.top_pad + content + s.bottom_pad == total);

                // 覆盖：视口内每一内容行都落在 [first, last] 某条消息内
                for (int row = row_lo; row <= row_hi; ++row) {
                    std::size_t idx = containing(prefix, row);
                    REQUIRE(idx >= s.first);
                    REQUIRE(idx <= s.last);
                }
            }
        }
    }
}

TEST_CASE("bottom scroll uses last messages with no bottom padding") {
    const auto prefix = make_prefix({1, 2, 4, 8, 3});
    const int total = prefix[5];
    int avail = 5;
    auto s = select_transcript_slice(prefix, total - avail, avail, 0);
    REQUIRE_FALSE(s.empty);
    REQUIRE(s.last == 4);        // 末条消息
    REQUIRE(s.bottom_pad == 0);  // 卷到底：无底留白
    // 顶留白 + 可见内容必须恰好铺满总体
    REQUIRE(s.top_pad + (prefix[5] - prefix[s.first]) == total);
}

TEST_CASE("large session: slice covers only a window of messages") {
    // 500 条消息，每条 1 行 → 总高 1000；视口 20，应只装配少数消息
    std::vector<int> heights(500, 1);
    const auto prefix = make_prefix(heights);
    int avail = 20, top = 700, margin = 8;
    auto s = select_transcript_slice(prefix, top, avail, margin);
    REQUIRE_FALSE(s.empty);
    const std::size_t covered = s.last - s.first + 1;
    REQUIRE(covered <= 40);  // 视口 + 缓冲范围内，远小于 500
    REQUIRE(s.top_pad >= 0);
    REQUIRE(s.bottom_pad >= 0);
    REQUIRE(s.top_pad + (prefix[s.last + 1] - prefix[s.first]) + s.bottom_pad == prefix[500]);
}