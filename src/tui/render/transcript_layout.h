/**
 * @file transcript_layout.h
 * @brief 转录区虚拟化的纯布局选择逻辑（与 UI 耦合为零，便于无头单测）
 * @details 仅依据逐消息前缀高度挑选可视切片，并给出顶/底留白行数。
 *          负责把「全部消息重渲染」收敛为「只装配可视切片」，从而让
 *          大历史会话的滚动/重绘成本与消息总数解耦。
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace ftxtui {

/// @brief 一次转录可视切片的选择结果
struct TranscriptSlice {
    std::size_t first = 0;  ///< 首个可见消息下标（含）；empty 时无效
    std::size_t last = 0;   ///< 末个可见消息下标（含）
    int top_pad = 0;        ///< 首可见消息之前的内容行数（顶留白）
    int bottom_pad = 0;     ///< 末可见消息之后的内容行数（底留白）
    bool empty = false;     ///< 无消息（空转录区）
};

/// @brief 依据逐消息前缀高度选择可视切片
/// @param prefix prefix[0]=0；prefix[k] = 前 k 条消息（含每条后的 1 行间距）
///               已占用的内容行数；prefix.size()==n+1，n 为消息数。
/// @param scroll_top 视口顶行（内容坐标，>=0）
/// @param avail 视口行数（>=1）
/// @param margin 视口上下额外装配的缓冲行数（>=0，消除边界抖动）
/// @return 切片覆盖 [scroll_top, scroll_top+avail) 至少以 margin 为缓冲；
///         且 top_pad+bottom_pad+内容高度 == 总内容高度 prefix[n]（保持滚动范围不变）
inline TranscriptSlice select_transcript_slice(const std::vector<int>& prefix, int scroll_top,
                                               int avail, int margin) {
    TranscriptSlice s;
    if (prefix.empty()) {
        s.empty = true;
        return s;
    }
    const std::size_t n = prefix.size() - 1;
    if (n == 0) {
        s.empty = true;
        return s;
    }
    if (avail < 1) avail = 1;
    if (margin < 0) margin = 0;
    if (scroll_top < 0) scroll_top = 0;

    const int total = prefix[n];
    const int row_lo = std::max(0, scroll_top - margin);
    const int row_hi = std::min(total - 1, scroll_top + avail - 1 + margin);
    if (row_lo >= total) {  // 卷过底部：装配尾部切片（内容全留白）
        s.empty = true;
        return s;
    }

    // 定位包含某内容行的消息：i 满足 prefix[i] <= row < prefix[i+1]
    auto msg_containing = [&](int row) -> std::size_t {
        if (row < 0) return 0;
        auto it = std::upper_bound(prefix.begin(), prefix.end(), row);
        std::size_t idx = static_cast<std::size_t>(it - prefix.begin()) - 1;
        if (idx >= n) idx = n - 1;
        return idx;
    };

    s.first = msg_containing(row_lo);
    s.last = msg_containing(row_hi);
    if (s.last < s.first) s.last = s.first;

    s.top_pad = prefix[s.first];
    s.bottom_pad = prefix[n] - prefix[s.last + 1];
    return s;
}

}  // namespace ftxtui