/**
 * @file text_wrap.h
 * @brief 按终端显示列宽折行（渲染与高度估算共用的唯一折行源）
 * @details FTXUI 的 text() 元素本身不折行：超过容器宽度会被截断/溢出。本模块
 *          提供 utf8 显示列宽与单词优先折行，把一条逻辑行切分成若干物理行
 *          （字节区间），供 build_markdown 渲染与 estimate_markdown_height
 *          计数共用，保证"单源真值"（A3）。CJK/全角字符按 2 列计宽。
 */

#pragma once

#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/screen/string.hpp>

namespace ftxtui {

/// @brief UTF-8 文本的终端显示列宽（CJK/全角=2，组合符≈0，其余=1）
/// @details 必须与 FTXUI text() 实际渲染的宽度判定完全一致（单源真值，A3）：
///          直接委托 ftxui::string_width（wcwidth 完整全角表，含 emoji 等宽区间），
///          否则折行/表格列宽按一个宽度表计算、渲染按另一个度量，
///          会造成表格边框错位、正文行超宽而越过内容区"黑边"。
inline int utf8_display_width(std::string_view s) { return ftxui::string_width(s); }

/// @brief 按显示列宽把一条逻辑行折行为若干物理行
/// @param text  原始逻辑行（UTF-8）
/// @param width 单行最大显示列宽（<=0 视为不折行，返回整行一段）
/// @return 每段为原 text 上的 [begin,end) 字节区间（互不重叠、顺序覆盖全文）
/// @details 单词优先：尽量在空白处断行；空白段在行首/行尾丢弃，单词间统一 1 列
///          分隔；单单词超过 width 时按列宽硬切（仅在码点边界切分）。
inline std::vector<std::pair<std::size_t, std::size_t>> wrap_text(std::string_view text,
                                                                  int width) {
    std::vector<std::pair<std::size_t, std::size_t>> rows;
    if (text.empty()) {
        rows.emplace_back(0u, 0u);
        return rows;
    }
    if (width <= 0) {
        rows.emplace_back(0u, text.size());
        return rows;
    }
    const int max_w = width;

    auto is_ws = [](unsigned char c) { return c == ' ' || c == '\t'; };

    // 把一段 [begin,end) 按列宽硬切成多个 [begin,end) 段
    auto hard_split = [&](std::size_t begin, std::size_t end) {
        std::vector<std::pair<std::size_t, std::size_t>> pieces;
        std::size_t start = begin;
        int col = 0;
        std::size_t i = begin;
        // 逐码点累加，超过 max_w 则切段
        while (i < end) {
            // 找当前字符的字节长度
            const unsigned char c0 = static_cast<unsigned char>(text[i]);
            int clen = 1;
            int extra = 0;
            if (c0 >= 0x80) {
                if ((c0 >> 5) == 0b110)
                    extra = 1;
                else if ((c0 >> 4) == 0b1110)
                    extra = 2;
                else if ((c0 >> 3) == 0b11110)
                    extra = 3;
                clen = 1 + extra;
            }
            const std::size_t token_end = std::min(end, i + static_cast<std::size_t>(clen));
            int cw = utf8_display_width(text.substr(i, token_end - i));
            if (col > 0 && col + cw > max_w) {
                pieces.emplace_back(start, i);
                start = i;
                col = 0;
            }
            col += cw;
            i = token_end;
        }
        if (col > 0 || pieces.empty()) pieces.emplace_back(start, end);
        return pieces;
    };

    const std::size_t n = text.size();
    std::size_t row_begin = n;  // 当前行起点（字节）
    int row_w = 0;              // 当前行已用列（不含行尾空白）
    bool row_has_word = false;

    auto flush_row = [&](std::size_t end_excl) {
        if (row_has_word) {
            // 去掉行尾空白：放不下下一词时 end_excl 落在词首，会把词前空白
            // 一并收进段尾，使物理行比计宽（单词间按 1 列）多 1 列而溢出。
            while (end_excl > row_begin && is_ws(static_cast<unsigned char>(text[end_excl - 1])))
                --end_excl;
            if (end_excl > row_begin) rows.emplace_back(row_begin, end_excl);
        }
    };

    // 扫描 token：空白段 或 单词段。空白仅作断行点（不计列），
    // 单词间统一按 1 列分隔计宽；行首/行尾空白自动丢弃。
    std::size_t begin = 0;
    while (begin < n) {
        const bool ws = is_ws(static_cast<unsigned char>(text[begin]));
        std::size_t end = begin;
        while (end < n && is_ws(static_cast<unsigned char>(text[end])) == ws) ++end;

        if (!ws) {
            const int tok_w = utf8_display_width(text.substr(begin, end - begin));
            if (tok_w > max_w) {
                // 超宽单词：无论当前是否已有内容，都先收尾当前行再硬切
                flush_row(begin);
                row_has_word = false;
                row_begin = n;
                row_w = 0;
                for (auto [b, e] : hard_split(begin, end)) rows.emplace_back(b, e);
            } else if (!row_has_word) {
                // 新行起始（此前空白被丢弃）
                row_begin = begin;
                row_w = tok_w;
                row_has_word = true;
            } else if (row_w + 1 + tok_w <= max_w) {
                // 行内能容纳：1 列单词间隔 + 词宽
                row_w += 1 + tok_w;
            } else {
                // 容纳不下：收尾当前行（词前空白一并丢弃），单词起新行
                flush_row(begin);
                row_begin = begin;
                row_w = tok_w;
                row_has_word = true;
            }
        }
        begin = end;
    }
    flush_row(n);

    return rows;
}

}  // namespace ftxtui