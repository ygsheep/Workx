/**
 * @file syntax_highlight.h
 * @brief 代码语法高亮（tree-sitter AST + 关键字回退）
 * @details 启用 tree-sitter（WORKX_HAS_TREE_SITTER）时，代码块走整块 AST
 *          解析（highlight_code_block），按语法节点类型着色；未启用或语言
 *          无 grammar 时回退到行内关键字分词（highlight_code_line）。
 *          不支持的或未标注的语言返回普通文本行，行为与之前一致。
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace ftxtui {

/// @brief 高亮一行代码（按语言；未知语言原样输出）
/// @param line 单行代码（不含换行）
/// @param lang 代码块标注语言（如 "cpp" / "python" / "js" / "bash"）
ftxui::Element highlight_code_line(std::string_view line, std::string_view lang);

/// @brief 高亮整块代码（tree-sitter 解析整块，返回每行一个 Element）
/// @param lines 代码块各行（不含换行）
/// @param lang 代码块标注语言
/// @return 与 lines 等长的 Element 列表；tree-sitter 不可用或语言不支持时
///         逐行回退到 highlight_code_line
std::vector<ftxui::Element> highlight_code_block(const std::vector<std::string>& lines,
                                                 std::string_view lang);

/// @brief 代码段着色区间（字节偏移，相对所在逻辑行起点）
struct HighlightSpan {
    uint32_t start = 0;  ///< 字节起点（含）
    uint32_t end = 0;    ///< 字节终点（不含）
    ftxui::Color color;
};

/// @brief 整块 tree-sitter 高亮，返回逐逻辑行的字节级 span（供折行渲染复用）
/// @param lines 代码各行（不含换行）
/// @param lang 代码块标注语言
/// @return 外层向量与 lines 等长，每项为该行 span 列表（字节偏移相对行首）；
///         tree-sitter 不可用或语言无 grammar 时返回空（调用方回退到
///         highlight_code_line 逐段处理）
std::vector<std::vector<HighlightSpan>> highlight_block_spans(const std::vector<std::string>& lines,
                                                              std::string_view lang);

/// @brief 渲染 text[byte_from, byte_to) 子串，并按 spans 对该子串着色
/// @param text 整行文本（与 spans 同基准）
/// @param byte_from 子串在该行的字节起点（含）
/// @param byte_to 子串在该行的字节终点（不含）
/// @param spans 该行的 span 列表（字节偏移相对行首）
/// @return 折行后某段的着色 Element
ftxui::Element render_spans_range(std::string_view text, uint32_t byte_from, uint32_t byte_to,
                                  const std::vector<HighlightSpan>& spans);

}  // namespace ftxtui
