#include "render/code_card.h"

#include <algorithm>
#include <string>

#include "render/syntax_highlight.h"
#include "theme/theme.h"

namespace ftxtui::codecard {
using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

int calc_line_num_width(int max_line_num) {
    if (max_line_num < 1) max_line_num = 1;
    int width = 1;
    for (int n = max_line_num; n >= 10; n /= 10) ++width;
    return width;
}

Element line_num_prefix(int line_num, int num_width) {
    const std::string box_v = "\u2502";
    const std::string num_str = std::to_string(line_num);
    const int pad = std::max(0, num_width - static_cast<int>(num_str.size()));
    return ftxui::color(theme::T::TextDim)(
        ftxui::text(box_v + std::string(pad, ' ') + num_str + " "));
}

namespace {

/// @brief 解析字符串开头的整数（忽略前导空白 / 逗号；支持负号）
int parse_leading_int(std::string_view s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == ',')) ++i;
    bool neg = false;
    if (i < s.size() && s[i] == '-') {
        neg = true;
        ++i;
    }
    int n = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        n = n * 10 + (s[i] - '0');
        ++i;
    }
    return neg ? -n : n;
}

}  // namespace

std::vector<DiffLine> parse_diff_lines(std::string_view diff) {
    std::vector<DiffLine> lines;
    size_t pos = 0;
    int old_cur = 0, new_cur = 0;
    bool have_hunk = false;
    while (pos <= diff.size()) {
        const size_t nl = diff.find('\n', pos);
        const std::string_view line =
            (nl == std::string_view::npos) ? diff.substr(pos) : diff.substr(pos, nl - pos);
        if (nl == std::string_view::npos)
            pos = diff.size() + 1;
        else
            pos = nl + 1;
        const bool header =
            (line.size() >= 3 && (line.substr(0, 3) == "---" || line.substr(0, 3) == "+++"));
        if (!header && line.size() >= 2 && line[0] == '@' && line[1] == '@') {
            old_cur = 0;
            new_cur = 0;
            have_hunk = false;
            const size_t a = line.find('-', 2);
            const size_t b =
                (a == std::string_view::npos) ? std::string_view::npos : line.find('+', a);
            if (a != std::string_view::npos && b != std::string_view::npos) {
                old_cur = parse_leading_int(line.substr(a + 1));
                new_cur = parse_leading_int(line.substr(b + 1));
                have_hunk = true;
            }
            continue;
        }
        if (!header && !line.empty()) {
            DiffLine dl;
            if (line[0] == '+') {
                dl.prefix = DiffPrefix::Add;
                if (have_hunk) dl.new_no = new_cur++;
                dl.content = std::string(line.substr(1));
            } else if (line[0] == '-') {
                dl.prefix = DiffPrefix::Del;
                if (have_hunk) dl.old_no = old_cur++;
                dl.content = std::string(line.substr(1));
            } else if (line[0] == ' ') {
                dl.content = std::string(line.substr(1));
                if (have_hunk) {
                    dl.old_no = old_cur++;
                    dl.new_no = new_cur++;
                }
            } else {
                dl.prefix = DiffPrefix::None;
                dl.content = std::string(line);
            }
            lines.push_back(std::move(dl));
        } else if (line.empty()) {
            lines.push_back({});
        }
    }
    return lines;
}

bool looks_like_diff(const std::vector<std::string>& lines) {
    for (const auto& l : lines) {
        if (l.compare(0, 3, "---") == 0 || l.compare(0, 3, "+++") == 0) return true;
        if (!l.empty() && l[0] == '@' && l.size() >= 2 && l[1] == '@') return true;
    }
    return false;
}

Element code_row(int disp_no, int num_width, std::string_view content, std::string_view lang,
                 ftxui::Color bg) {
    Element e = highlight_code_line(content, lang);
    if (bg != ftxui::Color::Black) e = e | ftxui::bgcolor(bg);
    Elements row;
    row.push_back(ftxui::text("  "));
    if (disp_no > 0)
        row.push_back(line_num_prefix(disp_no, num_width));
    else
        row.push_back(ftxui::text(""));
    row.push_back(ftxui::flex(std::move(e)));
    return ftxui::hbox(std::move(row));
}

Element build_code_card(const std::vector<std::string>& code_lines, std::string_view lang,
                        const std::vector<int>& line_nums) {
    int max_no = 0;
    for (const int n : line_nums) max_no = std::max(max_no, n);
    const int num_width = calc_line_num_width(max_no);
    // 统一走整块 tree-sitter 高亮（与主转录区一致；回退关键字按行）
    const std::vector<Element> hl = highlight_code_block(code_lines, lang);
    Elements rows;
    rows.reserve(code_lines.size());
    for (size_t i = 0; i < code_lines.size(); ++i) {
        const int no = (i < line_nums.size()) ? line_nums[i] : 0;
        Element content = (i < hl.size()) ? hl[i] : highlight_code_line(code_lines[i], lang);
        const Color bg = ftxui::Color::Black;
        if (bg != ftxui::Color::Black) content = content | ftxui::bgcolor(bg);
        Elements row;
        row.push_back(ftxui::text("  "));
        if (no > 0)
            row.push_back(line_num_prefix(no, num_width));
        else
            row.push_back(ftxui::text(""));
        row.push_back(ftxui::flex(std::move(content)));
        rows.push_back(ftxui::hbox(std::move(row)));
    }
    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme::T::Panel);
}

Color diff_row_background(DiffPrefix prefix) {
    if (prefix == DiffPrefix::Add) return Color::RGB(0x00, 0x5f, 0x00);
    if (prefix == DiffPrefix::Del) return Color::RGB(0x5f, 0x00, 0x00);
    return ftxui::Color::Black;
}

Element build_diff_card(const std::vector<DiffLine>& diff, std::string_view lang) {
    int max_line_num = 0;
    for (const auto& dl : diff) {
        const int n = (dl.prefix == DiffPrefix::Add && dl.new_no > 0)       ? dl.new_no
                      : (dl.prefix == DiffPrefix::Del && dl.old_no > 0)     ? dl.old_no
                      : (dl.prefix == DiffPrefix::Context && dl.new_no > 0) ? dl.new_no
                                                                            : 0;
        max_line_num = std::max(max_line_num, n);
    }
    const int num_width = calc_line_num_width(max_line_num);

    std::vector<std::string> contents;
    contents.reserve(diff.size());
    for (const auto& dl : diff) contents.push_back(dl.content);
    const std::vector<Element> hl = highlight_code_block(contents, lang);

    Elements rows;
    rows.reserve(diff.size());
    for (size_t i = 0; i < diff.size(); ++i) {
        const DiffLine& dl = diff[i];
        int disp_no = 0;
        if (dl.prefix == DiffPrefix::Add)
            disp_no = dl.new_no;
        else if (dl.prefix == DiffPrefix::Del)
            disp_no = dl.old_no;
        else if (dl.prefix == DiffPrefix::Context)
            disp_no = dl.new_no;
        Element content = (i < hl.size()) ? hl[i] : highlight_code_line(dl.content, lang);
        const Color bg = diff_row_background(dl.prefix);
        if (bg != ftxui::Color::Black) content = content | ftxui::bgcolor(bg);
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            disp_no > 0 ? line_num_prefix(disp_no, num_width) : ftxui::text(" "),
            ftxui::flex(content),
        }));
    }
    return ftxui::vbox(std::move(rows)) | ftxui::bgcolor(theme::T::Panel);
}

}  // namespace ftxtui::codecard