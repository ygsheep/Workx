/**
 * @file code_card.h
 * @brief 代码卡片 / diff 卡片渲染公共层
 * @details 从主输出区「文件卡片」工具结果渲染提炼：行号前缀、diff 文本解析、
 *          语法高亮 + 行号 + Panel 背景的组合，供主输出区工具结果与侧边栏
 *          「文件」tab 复用，避免侧栏重新实现一套卡片渲染。
 */

#pragma once

#include <ftxui/dom/elements.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace ftxtui::codecard {

/// @brief │N  行号列宽度（按最大行号位数，最少 1）
int calc_line_num_width(int max_line_num);

/// @brief │N 行号前缀（Dim 色，右对齐，尾随一空格）
ftxui::Element line_num_prefix(int line_num, int num_width);

// ---- diff 文本解析 ----

/// @brief diff 行前缀类型
enum class DiffPrefix : int { Add, Del, Context, None };

/// @brief 单行 diff 结构（解析后含真实行号）
struct DiffLine {
    DiffPrefix prefix = DiffPrefix::Context;
    std::string content;
    int old_no = 0;  ///< 真实旧文件行号（1-based；0=无对应行）
    int new_no = 0;  ///< 真实新文件行号（1-based；0=无对应行）
};

/// @brief 解析 diff 文本：解析 @@ -旧起,旧数 +新起,新数 @@ 头并给每行填真实行号；
///        跳过 ---/+++ 文件头，识别 +/-/空格 前缀
std::vector<DiffLine> parse_diff_lines(std::string_view diff);

/// @brief 内容是否像 diff 块（含 ---/+++ 文件头或 @@ hunk 头）
bool looks_like_diff(const std::vector<std::string>& lines);

// ---- 卡片构造（主输出区工具结果 + 侧栏文件查看共用）----

/// @brief 单行渲染：行号前缀 + 语法高亮内容（供虚拟化滚动逐行切片）
/// @param disp_no 显示行号（≤0 时不显示行号前缀）
/// @param bg 内容背景色（Color::Black = 无背景）
ftxui::Element code_row(int disp_no, int num_width, std::string_view content, std::string_view lang,
                        ftxui::Color bg = ftxui::Color::Black);

/// @brief 代码卡片整块：行号 + 语法高亮 + Panel 背景（→ 直接整块放入布局）
/// @param line_nums 每行显示行号（与 code_lines 等长；0=无行号）
ftxui::Element build_code_card(const std::vector<std::string>& code_lines, std::string_view lang,
                               const std::vector<int>& line_nums);

/// @brief diff 卡片整块：真实行号 + 前景高亮 + Add/Del 背景色 + Panel 背景
ftxui::Element build_diff_card(const std::vector<DiffLine>& diff, std::string_view lang);

/// @brief diff 行内容背景色（Add 绿 / Del 红 / 其余无背景）
ftxui::Color diff_row_background(DiffPrefix prefix);

}  // namespace ftxtui::codecard