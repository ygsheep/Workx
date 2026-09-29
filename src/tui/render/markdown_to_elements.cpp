#include "render/markdown_to_elements.h"
#include "render/syntax_highlight.h"
#include "render/text_wrap.h"
#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

namespace ftxtui {

using ftxui::BorderStyle;
using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 普通正文段落单行占用的行高（>=1）。中文字体行距 1.2 上下太挤，
///        TXTUI 每行高度只能取整数行，故折中定为 2：内容行 + 下方 1 空行，
///        让中文段落读起来更疏朗。仅作用于普通正文段落（含孤立 | 行），
///        标题/列表/代码块/表格保持紧凑，避免整体竖屏占用翻倍。
constexpr int kParagraphLineHeight = 2;

/// @brief 去首尾空白（空格/制表符），返回副本
std::string trim_copy(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

struct StyledSpan {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    Color fg;  // valid 由 fg_valid 表示
    bool fg_valid = false;
};

// 用户消息块配色：深色背景 + 左侧 accent 竖线
const Color kUserMsgBg = theme::T::Panel;
const Color kUserAccent = theme::T::Accent;

// 折叠卡片（思考/工具）圆角边框颜色
const Color kCardBorder = theme::T::TextFaint;

// 思考动画帧：Braille 旋转字符（复制自 src/tui/status_bar.cpp SPINNER_FRAMES）
constexpr const char* kSpinnerFrames[] = {
    "\u2813", "\u2819", "\u2839", "\u2838", "\u283C",
    "\u2834", "\u2826", "\u2827", "\u2807", "\u280F",
};
constexpr int kSpinnerFrameCount = 10;

/// @brief 进行中动画色：HSV 橙黄渐变（与 src/tui get_animated_color 同色相曲线）
Color spinner_color(std::size_t frame) {
    return Color::HSV(static_cast<uint8_t>(20 + (frame * 5) % 26), 255, 255);
}

/// @brief 从工具调用参数 JSON 提取文件路径（read_file/write_file/edit_file 的
///        file_path、grep 的 path），供工具卡展开首行展示
std::string tool_file_path(std::string_view args_json) {
    if (args_json.empty()) return {};
    try {
        const auto j = nlohmann::json::parse(args_json);
        for (const char* key : {"file_path", "path"}) {
            const auto it = j.find(key);
            if (it != j.end() && it->is_string()) return it->get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // 非 JSON 参数：无路径行
    }
    return {};
}

/// @brief 工具卡标题主题：文件工具→文件路径；命令工具（Bash/PowerShell/Shell）→命令
std::string tool_card_subject(std::string_view args_json) {
    if (args_json.empty()) return {};
    const std::string fpath = tool_file_path(args_json);
    if (!fpath.empty()) return fpath;
    try {
        const auto j = nlohmann::json::parse(args_json);
        for (const char* key : {"command", "cmd"}) {
            const auto it = j.find(key);
            if (it != j.end() && it->is_string()) return it->get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // 非 JSON 参数：无命令行
    }
    return {};
}

/// @brief Skill 工具参数中提取技能名（{\"name\":\"...\"}）；无法解析返回空
std::string skill_name_from_args(std::string_view args_json) {
    if (args_json.empty()) return {};
    try {
        const auto j = nlohmann::json::parse(args_json);
        const auto it = j.find("name");
        if (it != j.end() && it->is_string()) return it->get<std::string>();
    } catch (const nlohmann::json::exception&) {
        // 非 JSON 参数：无法解析技能名
    }
    return {};
}

/// @brief 按显示列宽（CJK=2）截断长文本，超出在末尾补省略号
std::string truncate_display(std::string_view s, int max_cols) {
    if (ftxui::string_width(s) <= max_cols) return std::string(s);
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c0 = static_cast<unsigned char>(s[i]);
        int len = 1;
        if (c0 >= 0x80) {
            if ((c0 >> 5) == 0b110)
                len = 2;
            else if ((c0 >> 4) == 0b1110)
                len = 3;
            else if ((c0 >> 3) == 0b11110)
                len = 4;
        }
        const int rw = ftxui::string_width(std::string_view(s.data() + i, len));
        // 省略号用 ASCII "..."（3 列）紧贴内容，避免 CJK 字体下 U+2026 渲染出前导空隙
        if (w + rw > max_cols - 3) {
            out += "...";
            break;
        }
        w += rw;
        out.append(s.data() + i, len);
        i += len;
    }
    return out;
}

// ============================================================================
// 工具结果特化渲染辅助（对齐 src/tui render_code_tool_result 的解析逻辑）
// ============================================================================

/// @brief 拆分 FileRead 行号行："  123→content" → 前缀 + 代码
void split_fileread_line(const std::string& line, std::string& line_prefix,
                         std::string& code_part) {
    line_prefix.clear();
    code_part = line;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const size_t num_start = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i == num_start) return;  // 无数字，不是行号格式
    if (i + 3 > line.size()) return;
    // 紧跟着 → (U+2192, UTF-8: E2 86 92)
    if (static_cast<unsigned char>(line[i]) != 0xE2 ||
        static_cast<unsigned char>(line[i + 1]) != 0x86 ||
        static_cast<unsigned char>(line[i + 2]) != 0x92)
        return;
    const size_t arrow_end = i + 3;
    line_prefix = line.substr(0, arrow_end);
    code_part = line.substr(arrow_end);
}

/// @brief 从 FileRead 行号前缀提取数字行号；无法解析返回 -1
int extract_fileread_line_number(const std::string& prefix) {
    size_t i = 0;
    while (i < prefix.size() && (prefix[i] == ' ' || prefix[i] == '\t')) ++i;
    if (i >= prefix.size() || prefix[i] < '0' || prefix[i] > '9') return -1;
    int num = 0;
    while (i < prefix.size() && prefix[i] >= '0' && prefix[i] <= '9') {
        num = num * 10 + (prefix[i] - '0');
        ++i;
    }
    return num;
}

/// @brief │N  行号前缀渲染宽度（按最大行号位数，最少 1）
int calc_line_num_width(int max_line_num) {
    if (max_line_num < 1) max_line_num = 1;
    int width = 1;
    for (int n = max_line_num; n >= 10; n /= 10) ++width;
    return width;
}

/// @brief FileRead 末尾元数据行（对齐 file_read_tool.cpp 实际格式）：
///        "(read lines N-M, more lines available)" / "(read lines N-M, total T, truncated)"
bool is_fileread_metadata_line(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.compare(i, 12, "(read lines ") == 0;
}

/// @brief diff 行前缀类型
enum class DiffPrefix : int { Add, Del, Context, None };
struct DiffLine {
    DiffPrefix prefix = DiffPrefix::Context;
    std::string content;
    int old_no = 0;  ///< 真实旧文件行号（1-based；0=无对应行）
    int new_no = 0;  ///< 真实新文件行号（1-based；0=无对应行）
};

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

/// @brief 解析 diff 文本：解析 @@ -旧起,旧数 +新起,新数 @@ 头并给每行填真实行号；
///        跳过 ---/+++ 文件头，识别 +/-/空格 前缀
std::vector<DiffLine> parse_diff_lines(std::string_view diff) {
    std::vector<DiffLine> lines;
    size_t pos = 0;
    int old_cur = 0, new_cur = 0;
    bool have_hunk = false;
    while (pos <= diff.size()) {
        const size_t nl = diff.find('\n', pos);
        const std::string_view line =
            (nl == std::string_view::npos) ? diff.substr(pos) : diff.substr(pos, nl - pos);
        // 取得当前行后立即推进 pos，保证下方所有 continue 分支都不会卡在同一行
        pos = (nl == std::string_view::npos) ? diff.size() + 1 : nl + 1;
        const bool header =
            (line.size() >= 3 && (line.substr(0, 3) == "---" || line.substr(0, 3) == "+++"));
        if (!header && line.size() >= 2 && line[0] == '@' && line[1] == '@') {
            // hunk 头（含多 hunk 时每 hunk 重开行号游标）
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
                dl.content = std::string(line.substr(1));
                if (have_hunk) dl.new_no = new_cur++;
            } else if (line[0] == '-') {
                dl.prefix = DiffPrefix::Del;
                dl.content = std::string(line.substr(1));
                if (have_hunk) dl.old_no = old_cur++;
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

/// @brief │N 行号前缀（Dim 色，右对齐，尾随一空格）
Element line_num_prefix(int line_num, int num_width) {
    const std::string box_v = "\u2502";
    const std::string num_str = std::to_string(line_num);
    const int pad = std::max(0, num_width - static_cast<int>(num_str.size()));
    return ftxui::color(theme::T::TextDim)(
        ftxui::text(box_v + std::string(pad, ' ') + num_str + " "));
}

/// @brief 工具结果展开的最大显示行数（对齐 src/tui MAX_DISPLAY_LINES）
constexpr int kMaxToolResultLines = 60;

/// @brief 用户消息装饰器：深色背景（#1e1e1e）+ 左侧 accent 竖线（无边框盒）
/// @details 先渲染子节点再铺背景/边框，避免覆盖文字前景色
class UserMessageBox : public ftxui::Node {
   public:
    explicit UserMessageBox(Element child) : Node(Elements{std::move(child)}) {}

    void ComputeRequirement() override {
        children_[0]->ComputeRequirement();
        requirement_ = children_[0]->requirement();
        requirement_.min_x += 1;  // 左边框列
    }

    void SetBox(ftxui::Box box) override {
        box_ = box;
        children_[0]->SetBox(ftxui::Box{box.x_min + 1, box.x_max, box.y_min, box.y_max});
    }

    void Render(ftxui::Screen& screen) override {
        children_[0]->Render(screen);
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min + 1; x <= box_.x_max; ++x) {
                screen.PixelAt(x, y).background_color = kUserMsgBg;
            }
            auto& p = screen.PixelAt(box_.x_min, y);
            p.character = "\u2502";  // │
            p.foreground_color = kUserAccent;
            p.background_color = kUserMsgBg;
        }
    }
};

// ---- 行内解析：把 `**bold**`, `*it*`, `~~del~~`, `` `code` `` 拆为 StyledSpan 序列 ----
std::vector<StyledSpan> parse_inline_spans(std::string_view text) {
    std::vector<StyledSpan> out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            StyledSpan s;
            s.text = std::move(cur);
            s.code = true;
            cur.clear();
            out.push_back(std::move(s));
        }
    };
    size_t i = 0;
    while (i < text.size()) {
        // 代码：`...`
        if (text[i] == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                StyledSpan s;
                s.text.assign(text.substr(i + 1, end - i - 1));
                s.code = true;
                out.push_back(std::move(s));
                i = end + 1;
                continue;
            }
        }
        if (i + 2 < text.size() && text[i] == '*' && text[i + 1] == '*' && text[i + 2] == '*') {
            size_t end = text.find("***", i + 3);
            if (end != std::string_view::npos) {
                // 粗斜体：拆分内层
                StyledSpan s;
                s.text.assign(text.substr(i + 3, end - i - 3));
                s.bold = true;
                s.italic = true;
                out.push_back(std::move(s));
                i = end + 3;
                continue;
            }
        }
        if (i + 1 < text.size() && text[i] == '*' && text[i + 1] == '*') {
            size_t end = text.find("**", i + 2);
            if (end != std::string_view::npos && end > i + 1) {
                StyledSpan s;
                s.text.assign(text.substr(i + 2, end - i - 2));
                s.bold = true;
                out.push_back(std::move(s));
                i = end + 2;
                continue;
            }
        }
        if (i + 1 < text.size() && text[i] == '~' && text[i + 1] == '~') {
            size_t end = text.find("~~", i + 2);
            if (end != std::string_view::npos) {
                StyledSpan s;
                s.text.assign(text.substr(i + 2, end - i - 2));
                s.strike = true;
                out.push_back(std::move(s));
                i = end + 2;
                continue;
            }
        }
        if (text[i] == '*') {
            size_t end = text.find('*', i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                StyledSpan s;
                s.text.assign(text.substr(i + 1, end - i - 1));
                s.italic = true;
                out.push_back(std::move(s));
                i = end + 1;
                continue;
            }
        }
        // 转义
        if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i + 1] == '*' || text[i + 1] == '_' || text[i + 1] == '`' ||
             text[i + 1] == '~')) {
            cur.push_back(text[i + 1]);
            i += 2;
            continue;
        }
        cur.push_back(text[i]);
        ++i;
    }
    // 收尾普通文本（非 code）
    if (!cur.empty()) {
        StyledSpan s;
        s.text = std::move(cur);
        out.push_back(std::move(s));
    }
    if (out.empty()) {
        StyledSpan s;
        s.text = "";
        out.push_back(std::move(s));  // 保证至少一个 span，段落可为空行
    }
    return out;
}

Element span_element(const StyledSpan& s) {
    Element e = ftxui::text(s.text);
    if (s.bold) e = e | ftxui::bold;
    if (s.italic) e = e | ftxui::color(theme::T::Text);
    if (s.strike) e = e | ftxui::strikethrough;
    if (s.fg_valid) e = e | ftxui::color(s.fg);
    if (s.code) e = e | ftxui::color(theme::T::Text);
    return e;
}

/// @brief 渲染 span 序列为横向元素（保留粗体/代码等样式）
Element spans_to_element(const std::vector<StyledSpan>& spans) {
    Elements children;
    children.reserve(spans.size());
    for (const auto& s : spans) children.push_back(span_element(s));
    return ftxui::hbox(std::move(children));
}

Element line_to_element(std::string_view line) {
    return spans_to_element(parse_inline_spans(line));
}

bool is_code_fence(std::string_view line, std::string& lang_out) {
    if (line.size() < 3 || line.substr(0, 3) != "```") return false;
    lang_out.assign(line.substr(3));
    // trim
    while (!lang_out.empty() && (lang_out.front() == ' ' || lang_out.front() == '\t'))
        lang_out.erase(lang_out.begin());
    while (!lang_out.empty() && std::isspace(static_cast<unsigned char>(lang_out.back())))
        lang_out.pop_back();
    return true;
}

bool is_hr(std::string_view line) {
    std::string t(line.begin(), line.end());
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char ch : t)
        if (ch != c && ch != ' ') return false;
    return true;
}

bool is_list_item(std::string_view line, std::string& content_out, bool& ordered_out) {
    std::string t(line.begin(), line.end());
    size_t indent = 0;
    while (indent < t.size() && (t[indent] == ' ' || t[indent] == '\t')) ++indent;
    t = t.substr(indent);
    if (t.empty()) return false;
    // 无序 - / * / +
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        content_out = t.substr(2);
        ordered_out = false;
        return true;
    }
    // 有序 N.
    size_t dot = t.find('.');
    if (dot != std::string::npos && dot + 1 < t.size() && t[dot + 1] == ' ') {
        bool digits = true;
        for (size_t k = 0; k < dot; ++k)
            if (!std::isdigit(static_cast<unsigned char>(t[k]))) {
                digits = false;
                break;
            }
        if (digits) {
            content_out = t.substr(dot + 2);
            ordered_out = true;
            return true;
        }
    }
    return false;
}

bool is_heading(std::string_view line, int& level_out, std::string_view& content_out) {
    size_t n = 0;
    while (n < line.size() && line[n] == '#') ++n;
    if (n < 1 || n > 6 || n >= line.size() || line[n] != ' ') return false;
    level_out = static_cast<int>(n);
    content_out = line.substr(n + 1);
    return true;
}

bool is_table_row(std::string_view line) {
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    return !t.empty() && t.front() == '|';
}

/// @brief 拆分表格行（| a | b | → [a, b]；支持 \| 转义与首尾竖线）
std::vector<std::string> split_table_row(std::string_view line);

/// @brief 表格分隔行（| --- | :---: | 等），并解析各列对齐（0=左 1=中 2=右）
bool is_table_separator(std::string_view line, std::vector<int>& aligns_out) {
    aligns_out.clear();
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    if (t.empty() || t.front() != '|') return false;

    auto cells = split_table_row(t);
    for (const auto& cell : cells) {
        std::string c = cell;
        while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) c.erase(c.begin());
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.pop_back();
        if (c.empty()) return false;
        bool starts_colon = c.front() == ':';
        bool ends_colon = c.back() == ':';
        bool has_dash = false;
        for (char ch : c) {
            if (ch == '-')
                has_dash = true;
            else if (ch != ':' && ch != ' ' && ch != '\t')
                return false;
        }
        if (!has_dash) return false;
        aligns_out.push_back(starts_colon && ends_colon ? 1
                             : starts_colon             ? 0
                             : ends_colon               ? 2
                                                        : 0);
    }
    return true;
}

/// @brief 拆分表格行（| a | b | → [a, b]；支持 \| 转义与首尾竖线）
std::vector<std::string> split_table_row(std::string_view line) {
    std::vector<std::string> cells;
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);

    std::string cur;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (c == '|') {
            cells.push_back(trim_copy(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.push_back(trim_copy(cur));
    return cells;
}

/// @brief 单元格显示宽度：先剥离行内标记（**bold** → bold），与 line_to_element 渲染对齐
int cell_display_width(std::string_view cell) {
    int w = 0;
    for (const auto& s : parse_inline_spans(cell)) w += ftxui::string_width(s.text);
    return w;
}

/// @brief 表格单元格折行：剥离行内标记后按显示列宽折行，再按样式切回 span
/// @details wrap_text 按原始字节计宽会把行内标记（** 等）算进宽度，导致带标记的
///          单元格在列宽恰好等于显示宽时被提前折行；这里先拼纯文本（去标记）折行，
///          再把每个物理行切回原始 span 序列，保留粗体/代码等样式。
/// @return 每行一个 span 序列；空单元格返回 1 行空 span
std::vector<std::vector<StyledSpan>> wrap_cell_spans(std::string_view cell, int col_w) {
    const std::vector<StyledSpan> spans = parse_inline_spans(cell);
    std::string plain;
    std::vector<std::size_t> starts;
    starts.reserve(spans.size());
    for (const auto& s : spans) {
        starts.push_back(plain.size());
        plain += s.text;
    }
    const auto segs = wrap_text(plain, std::max(1, col_w));
    std::vector<std::vector<StyledSpan>> out;
    out.reserve(segs.size());
    for (auto [b, e] : segs) {
        std::vector<StyledSpan> line;
        std::size_t idx = 0;
        while (idx + 1 < spans.size() && starts[idx + 1] <= b) ++idx;
        std::size_t pos = b;
        while (pos < e && idx < spans.size()) {
            const std::size_t span_end = (idx + 1 < spans.size()) ? starts[idx + 1] : plain.size();
            const std::size_t seg_b = pos;
            const std::size_t seg_e = std::min(e, span_end);
            if (seg_e > seg_b) {
                StyledSpan s = spans[idx];
                s.text = plain.substr(seg_b, seg_e - seg_b);
                line.push_back(std::move(s));
            }
            pos = seg_e;
            ++idx;
        }
        if (line.empty()) line.push_back(StyledSpan{});
        out.push_back(std::move(line));
    }
    return out;
}

/// @brief 计算表格列宽：自然宽（各列最大显示宽）→ 按可用宽度封顶
/// @param header 表头单元格
/// @param data   数据行单元格
/// @param max_width 表格可用总列宽（含边框）；<=0 时不封顶
/// @return 各列显示宽（至少 2，容纳 CJK 全角字符）
std::vector<int> compute_table_widths(const std::vector<std::string>& header,
                                      const std::vector<std::vector<std::string>>& data,
                                      int max_width) {
    size_t cols = header.size();
    for (const auto& r : data) cols = std::max(cols, r.size());
    std::vector<int> widths(cols, 2);
    for (size_t c = 0; c < header.size(); ++c)
        widths[c] = std::max(widths[c], cell_display_width(header[c]));
    for (const auto& r : data)
        for (size_t c = 0; c < r.size(); ++c)
            widths[c] = std::max(widths[c], cell_display_width(r[c]));

    if (max_width > 0) {
        // 表格总宽 = 1 + Σwidth + 3*cols（边框+列间距），封顶到可用宽度
        const int avail =
            std::max(static_cast<int>(cols), max_width - 1 - 3 * static_cast<int>(cols));
        long total = 0;
        for (int w : widths) total += w;
        if (total > avail) {
            const std::vector<int> natural = widths;
            // 按自然宽比例收缩（至少 2 列）
            for (int& w : widths)
                w = std::max(2, static_cast<int>(w * static_cast<long>(avail) / total));
            // 修正比例取整造成的超出：从最宽列逐列削减（极端窄屏可低于 2）
            total = 0;
            for (int w : widths) total += w;
            while (total > avail) {
                int idx = -1;
                for (size_t c = 0; c < widths.size(); ++c)
                    if (widths[c] > 1 && (idx < 0 || widths[c] > widths[idx]))
                        idx = static_cast<int>(c);
                if (idx < 0) break;
                --widths[idx];
                --total;
            }
            // 回补窄列：比例收缩会把窄列（如表头）挤到 2 列导致换行，
            // 把低于 min(自然宽,4) 的列从最宽列扣减补足（总宽不变）
            for (size_t c = 0; c < widths.size(); ++c) {
                const int floor = std::min(natural[c], 4);
                while (widths[c] < floor) {
                    int donor = -1;
                    for (size_t d = 0; d < widths.size(); ++d)
                        if (widths[d] > 2 && (donor < 0 || widths[d] > widths[donor]))
                            donor = static_cast<int>(d);
                    if (donor < 0) break;
                    --widths[donor];
                    ++widths[c];
                }
            }
        }
    }
    return widths;
}

/// @brief 表格行渲染高度：各列折行行数的最大值（至少 1）
int table_row_height(const std::vector<std::string>& cells, const std::vector<int>& widths) {
    int h = 1;
    for (size_t c = 0; c < cells.size(); ++c) {
        const int w = (c < widths.size()) ? widths[c] : 1;
        h = std::max(h, static_cast<int>(wrap_cell_spans(cells[c], w).size()));
    }
    return h;
}

/// @brief 表格块渲染（FTXUI 版，样式对齐 src/tui render_table）
/// @param rows 原始表格行：rows[0]=表头，rows[1]=分隔行，rows[2..]=数据行
/// @param max_width 表格可用总列宽（含边框）；长单元格按列宽折行，避免溢出输出区
Element render_table_block(const std::vector<std::string>& rows, int max_width) {
    const std::vector<std::string> header = split_table_row(rows[0]);
    std::vector<int> aligns;
    is_table_separator(rows[1], aligns);
    aligns.resize(header.size(), 0);

    std::vector<std::vector<std::string>> data;
    for (size_t i = 2; i < rows.size(); ++i) data.push_back(split_table_row(rows[i]));

    const std::vector<int> widths = compute_table_widths(header, data, max_width);
    const size_t cols = widths.size();
    aligns.resize(cols, 0);

    auto border = [&](const char* left, const char* mid, const char* right) {
        std::string s = left;
        for (size_t c = 0; c < cols; ++c) {
            if (c > 0) s += mid;
            for (int k = 0; k < widths[c] + 2; ++k) s += "\u2500";
        }
        s += right;
        return ftxui::text(s);
    };

    // 单元格：按列宽折行（保留行内样式），每物理行按对齐方式补空格。
    // 返回各物理行元素（每行宽度 = col_w），供行级逐行拼装以补全 │ 分隔线。
    auto cell_lines = [&](const std::string& cell, int col_w, int align) {
        auto lines = wrap_cell_spans(cell, col_w);
        std::vector<Element> out;
        out.reserve(lines.size());
        for (const auto& line : lines) {
            int w = 0;
            for (const auto& s : line) w += ftxui::string_width(s.text);
            int pad = std::max(0, col_w - w);
            int left = 0, right = pad;
            if (align == 1) {
                left = pad / 2;
                right = pad - left;
            } else if (align == 2) {
                left = pad;
                right = 0;
            }
            Elements children;
            if (left > 0) children.push_back(ftxui::text(std::string(left, ' ')));
            for (const auto& s : line) children.push_back(span_element(s));
            if (right > 0) children.push_back(ftxui::text(std::string(right, ' ')));
            out.push_back(ftxui::hbox(std::move(children)));
        }
        if (out.empty()) out.push_back(ftxui::text(std::string(col_w, ' ')));
        return out;
    };

    // 行：各列折行行数取最大为行高，逐物理行拼装 hbox，
    // 短列补空白行 → 换行后的每一行都有完整 │ 分隔线
    auto row_elem = [&](const std::vector<std::string>& cells, bool head) {
        std::vector<std::vector<Element>> cols_lines;
        cols_lines.reserve(cols);
        int row_h = 1;
        for (size_t c = 0; c < cols; ++c) {
            std::string cell = c < cells.size() ? cells[c] : "";
            auto lines = cell_lines(cell, widths[c], aligns[c]);
            if (head) {
                for (auto& e : lines) e = e | ftxui::bold;
            }
            row_h = std::max(row_h, static_cast<int>(lines.size()));
            cols_lines.push_back(std::move(lines));
        }
        Elements rows_out;
        rows_out.reserve(static_cast<size_t>(row_h));
        for (int k = 0; k < row_h; ++k) {
            Elements children;
            children.push_back(ftxui::text("\u2502 "));
            for (size_t c = 0; c < cols; ++c) {
                if (c > 0) children.push_back(ftxui::text(" \u2502 "));
                const auto& lines = cols_lines[c];
                if (k < static_cast<int>(lines.size()))
                    children.push_back(lines[static_cast<size_t>(k)]);
                else
                    children.push_back(ftxui::text(std::string(widths[c], ' ')));
            }
            children.push_back(ftxui::text(" \u2502"));
            rows_out.push_back(ftxui::hbox(std::move(children)));
        }
        return ftxui::vbox(std::move(rows_out));
    };

    Elements rows_elem;
    rows_elem.push_back(border("\u250c", "\u252c", "\u2510"));  // ┌ ┬ ┐
    rows_elem.push_back(row_elem(header, true));
    rows_elem.push_back(border("\u251c", "\u253c", "\u2524"));  // ├ ┼ ┤
    for (const auto& r : data) rows_elem.push_back(row_elem(r, false));
    rows_elem.push_back(border("\u2514", "\u2534", "\u2518"));  // └ ┴ ┘
    return ftxui::vbox(std::move(rows_elem));
}

// ---- 消息各嵌套层级的正文可用宽（build_transcript 外层缩进 2 恒存在） ----
/// @brief 助手正文可用宽：外层 2 + build_message 内层缩进 2
inline int message_body_width(int width) { return std::max(1, width - 4); }
/// @brief 用户消息正文可用宽：外层 2 + 左边框 1 + 内层缩进 2
inline int user_body_width(int width) { return std::max(1, width - 5); }
/// @brief 卡片（思考/工具）内容可用宽：外层 2 + 卡片边框 2 + 内容缩进 4
inline int card_content_width(int width) { return std::max(1, width - 8); }

/// @brief 按 '\n' 拆行（与 build_markdown 的行拆分一致）
std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else
            cur.push_back(c);
    }
    lines.push_back(std::move(cur));
    return lines;
}

/// @brief 统计 Markdown 块级渲染行数（A3：与 build_markdown 布局一一对应）
/// @details 与渲染约定的对应关系：
///          - 空行 → emptyElement（min_y=0，不占行）
///          - 标题/分隔线/列表/段落 → 文本按显示宽度折行后的行数
///          - 代码块 → 每代码行 1 行（不折行）+ 语言标签 1 行（仅当有内容时渲染）
///          - 表格  → 表头+分隔+数据行固定布局
/// @param width 正文折行的单行最大显示列宽（列表项额外减 4 列子弹缩进）
/// @param compact 紧凑模式：普通段落每物理行计 1 行（不做 ×kParagraphLineHeight）
int count_markdown_lines(const std::vector<std::string>& lines, int width, bool compact) {
    const int safe_w = std::max(1, width);
    int h = 0;
    bool in_code = false;
    std::string code_lang;
    std::vector<std::string> code_lines;

    auto flush_code = [&]() {
        if (in_code && !code_lines.empty()) {
            h += static_cast<int>(code_lines.size());
            if (!code_lang.empty()) ++h;  // 语言标签行
            // 上下留白：非紧凑（assistant 等）各 1 行（vPad）；紧凑（用户消息）不追加，
            // 与 build_markdown flush_code 的 compact 分支保持一致
            if (!compact) h += 2;
        }
        in_code = false;
        code_lang.clear();
        code_lines.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (!in_code) {
                in_code = true;
                (void)is_code_fence(line, code_lang);
                continue;
            }
            flush_code();
            continue;
        }
        if (in_code) {
            code_lines.push_back(line);
            continue;
        }
        // 表格块：表头行 + 分隔行 + 连续 | 行 → 顶/表头/中/底边框 + 数据行
        // （行高按列宽折行统计，与 render_table_block 一致）
        std::vector<int> aligns_tmp;
        if (is_table_row(line) && i + 1 < lines.size() &&
            is_table_separator(lines[i + 1], aligns_tmp)) {
            size_t j = i + 2;
            while (j < lines.size() && is_table_row(lines[j])) ++j;
            const std::vector<std::string> header = split_table_row(lines[i]);
            std::vector<std::vector<std::string>> data;
            for (size_t k = i + 2; k < j; ++k) data.push_back(split_table_row(lines[k]));
            const std::vector<int> widths = compute_table_widths(header, data, width);
            h += 3;  // 顶/中/底边框
            h += table_row_height(header, widths);
            for (const auto& r : data) h += table_row_height(r, widths);
            i = j - 1;
            continue;
        }
        // 空行/全空白：emptyElement（min_y=0）不占行
        bool blank = line.empty() || std::all_of(line.begin(), line.end(),
                                                 [](char c) { return c == ' ' || c == '\t'; });
        if (blank) continue;
        // 标题/列表/普通行：按 build_markdown 的折行宽度统计（单源真值）
        int level = 0;
        std::string_view heading;
        std::string content;
        bool ordered = false;
        if (is_heading(line, level, heading))
            h += static_cast<int>(wrap_text(heading, safe_w).size());
        else if (is_list_item(line, content, ordered))
            h += static_cast<int>(wrap_text(content, std::max(1, safe_w - 4)).size());
        else if (is_hr(line))
            h += 1;  // 水平分隔线：恒定 1 行，与 build_markdown 的 separator 一致（不走行距）
        else
            // 普通段落：compact 模式每物理行占 1 行，否则与 paragraph_block 一致
            // 每物理行占 kParagraphLineHeight 行高
            h += (compact ? 1 : kParagraphLineHeight) *
                 static_cast<int>(wrap_text(line, safe_w).size());
    }
    flush_code();
    return h;
}

}  // namespace

/// @brief 文件路径扩展名 → ftxtui 支持的语法高亮语言标签；未知返回空
/// @details 供工具卡 / 文件查看器（/view）复用同一套扩展名映射
std::string lang_from_path(const std::string& path) {
    if (path.empty()) return {};
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name == "CMakeLists.txt") return "cpp";
    if (name == "Dockerfile") return "bash";
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "c" || ext == "h") return "c";
    if (ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "hpp" || ext == "hxx" || ext == "ino")
        return "cpp";
    if (ext == "py" || ext == "pyw" || ext == "pyi") return "python";
    if (ext == "js" || ext == "jsx" || ext == "mjs" || ext == "cjs") return "js";
    if (ext == "ts" || ext == "tsx") return "ts";
    if (ext == "rs") return "rust";
    if (ext == "go") return "go";
    if (ext == "sh" || ext == "bash" || ext == "zsh" || ext == "ksh") return "bash";
    if (ext == "json") return "json";
    if (ext == "yaml" || ext == "yml") return "yaml";
    if (ext == "sql") return "sql";
    return {};
}

/// @brief 把一段文本按显示宽度折行并渲染为纵向块（每物理行一个行内元素）
/// @param src    渲染源文本（含行内标记）
/// @param wrap_w 单行最大显示列宽
/// @param decorate 可选：对每个物理行元素施加样式（如标题加粗）
Element wrap_block(std::string_view src, int wrap_w,
                   const std::function<Element(Element)>& decorate = {}) {
    auto rows = wrap_text(src, std::max(1, wrap_w));
    Elements es;
    es.reserve(rows.size());
    for (auto [b, e] : rows) {
        Element r = line_to_element(src.substr(b, e - b));
        if (decorate) r = decorate(std::move(r));
        es.push_back(std::move(r));
    }
    return ftxui::vbox(std::move(es));
}

/// @brief 普通正文段落：每个物理行占 kParagraphLineHeight 行高，放大中文字体行距。
///        标题/列表/代码块/表格不走这里（保持紧凑），避免整体竖屏占用翻倍。
Element paragraph_block(std::string_view src, int wrap_w) {
    auto rows = wrap_text(src, std::max(1, wrap_w));
    Elements es;
    es.reserve(rows.size());
    for (auto [b, e] : rows) {
        es.push_back(ftxui::flex(line_to_element(src.substr(b, e - b))) |
                     ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, kParagraphLineHeight));
    }
    return ftxui::vbox(std::move(es));
}

Element build_markdown(std::string_view text, int width, bool compact) {
    const int safe_w = std::max(1, width);
    if (text.empty()) return ftxui::text("");

    // 紧凑段落：每物理行占 1 行（不做 ×kParagraphLineHeight），避免段落尾随空行，
    // 供用户消息使用——块高即文本行数，与转录区统一间距叠加后不产生"多一行"。
    auto paragraph = [&](std::string_view line) {
        return compact ? wrap_block(line, safe_w) : paragraph_block(line, safe_w);
    };

    const std::vector<std::string> lines = split_lines(text);

    Elements blocks;
    bool in_code = false;
    std::string code_lang;
    std::vector<std::string> code_lines;

    auto flush_code = [&]() {
        if (in_code && !code_lines.empty()) {
            // 代码块：深色背景块（#1e1e1e），无边框线；行内 flex 铺满消息宽度
            const std::vector<Element> hl = highlight_code_block(code_lines, code_lang);
            Elements code_rows;
            code_rows.reserve(hl.size());
            for (const auto& e : hl) {
                code_rows.push_back(ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::flex(e),
                }));
            }
            auto code_elem = ftxui::vbox(std::move(code_rows));
            if (!code_lang.empty()) {
                auto lang_elem = ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::flex(ftxui::color(theme::T::Text)(ftxui::text(code_lang))),
                });
                code_elem = ftxui::vbox({lang_elem, code_elem});
            }
            // 代码块上下留白：默认（assistant 等）维持 vPad（README「代码上下一行距离」），
            // compact（用户消息）不追加任何留白——用户消息块不带内边距，
            // 上下间距完全由转录音区每条消息后统一追加的 text(" ") 承担，
            // 避免代码块底部空行与转录区间距叠加成"下面多一行"。
            auto code_bg = code_elem | ftxui::bgcolor(theme::T::Panel);
            blocks.push_back(compact ? std::move(code_bg) : theme::vPad(std::move(code_bg)));
        }
        in_code = false;
        code_lang.clear();
        code_lines.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (!in_code) {
                in_code = true;
                (void)is_code_fence(line, code_lang);
                continue;
            } else {
                flush_code();
                continue;
            }
        }
        if (in_code) {
            code_lines.push_back(line);
            continue;
        }

        // 空行
        if (line.empty() ||
            std::all_of(line.begin(), line.end(), [](char c) { return c == ' ' || c == '\t'; })) {
            blocks.push_back(ftxui::emptyElement());
            continue;
        }

        int level = 0;
        std::string_view heading;
        if (is_heading(line, level, heading)) {
            auto e = wrap_block(heading, safe_w, [](Element x) { return x | ftxui::bold; });
            if (level >= 5) e = e | ftxui::color(theme::T::Text);
            blocks.push_back(e);
            continue;
        }

        if (is_hr(line)) {
            blocks.push_back(ftxui::separator());
            continue;
        }

        std::string content;
        bool ordered = false;
        if (is_list_item(line, content, ordered)) {
            (void)ordered;
            // 列表项：首行 "  • " 前缀，续行按内容列缩进（4 格）；折行减前缀宽
            auto rows = wrap_text(content, std::max(1, safe_w - 4));
            Elements es;
            es.reserve(rows.size());
            for (size_t k = 0; k < rows.size(); ++k) {
                const char* prefix = (k == 0) ? "  \u2022 " : "    ";
                es.push_back(ftxui::hbox({
                    ftxui::text(prefix),
                    ftxui::flex(line_to_element(
                        content.substr(rows[k].first, rows[k].second - rows[k].first))),
                }));
            }
            blocks.push_back(ftxui::vbox(std::move(es)));
            continue;
        }

        // 表格块：表头行 + 分隔行 + 连续 | 行（样式对齐 src/tui render_table）
        std::vector<int> aligns_tmp;
        if (is_table_row(line) && i + 1 < lines.size() &&
            is_table_separator(lines[i + 1], aligns_tmp)) {
            std::vector<std::string> tbl;
            tbl.push_back(line);
            tbl.push_back(lines[++i]);  // 分隔行
            while (i + 1 < lines.size() && is_table_row(lines[i + 1])) tbl.push_back(lines[++i]);
            blocks.push_back(render_table_block(tbl, safe_w));
            continue;
        }

        // 孤立 | 行（无分隔行）：降级为普通行内文本（保留 | 分隔，可读）
        if (is_table_row(line)) {
            blocks.push_back(ftxui::color(theme::T::Text)(paragraph(line)));
            continue;
        }

        // 普通段落：按显示宽度折行；默认行距放大（中文字体可读性），compact 模式每行 1 行
        blocks.push_back(paragraph(line));
    }
    flush_code();
    return ftxui::vbox(std::move(blocks));
}

Element build_inline_line(std::string_view line) { return ftxui::flex(line_to_element(line)); }

/// @brief 渲染工具结果内容（对齐 src/tui render_code_tool_result）：
///   - Read：状态行 + 代码高亮 + │N 行号 + 元数据行 + 截断
///   - Write/Edit：状态文本（Dim）+ diff（前景高亮 + +/- 背景色 + │N 序号）+ 截断
///   - Shell 工具（Bash/PowerShell/Shell）：剥离 <stdout>/<stderr>/<error> 标签，
///     <error> 内容红色显示
///   - 其他工具/错误：通用 markdown（原行为）
/// @details 代码与 diff 区域统一 Panel 背景 + 上下留白（theme::vPad），
///          与 build_markdown 代码块视觉一致（README「代码上下一行距离」）。

// ============================================================================
// Shell 工具结果（Bash/PowerShell/Shell）：剥离 <stdout>/<stderr>/<error> 标签
// ============================================================================

enum class ShellSectionKind { Error, Stdout, Stderr, Plain };

struct ShellSection {
    ShellSectionKind kind = ShellSectionKind::Plain;
    std::string text;
};

/// @brief 解析 shell 工具结果中的 <error>/<stdout>/<stderr> 标签段
std::vector<ShellSection> parse_shell_sections(std::string_view s) {
    struct Tag {
        std::string_view open, close;
        ShellSectionKind kind = ShellSectionKind::Plain;
    };
    static constexpr Tag kTags[] = {
        {"<error>", "</error>", ShellSectionKind::Error},
        {"<stdout>", "</stdout>", ShellSectionKind::Stdout},
        {"<stderr>", "</stderr>", ShellSectionKind::Stderr},
    };
    std::vector<ShellSection> out;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t best = s.npos;
        const Tag* best_tag = nullptr;
        for (const auto& tag : kTags) {
            const std::size_t p = s.find(tag.open, pos);
            if (p != s.npos && p < best) {
                best = p;
                best_tag = &tag;
            }
        }
        if (!best_tag) {
            const std::string rest(s.substr(pos));
            if (!rest.empty()) out.push_back({ShellSectionKind::Plain, rest});
            break;
        }
        if (best > pos)
            out.push_back({ShellSectionKind::Plain, std::string(s.substr(pos, best - pos))});
        const std::size_t content_begin = best + best_tag->open.size();
        const std::size_t close = s.find(best_tag->close, content_begin);
        std::string content;
        if (close == s.npos) {
            content = std::string(s.substr(content_begin));
            pos = s.size();
        } else {
            content = std::string(s.substr(content_begin, close - content_begin));
            pos = close + best_tag->close.size();
        }
        // 格式为 "<tag>\n...\n</tag>"：去掉首尾各一个换行
        if (!content.empty() && content.front() == '\n') content.erase(content.begin());
        if (!content.empty() && content.back() == '\n') content.pop_back();
        out.push_back({best_tag->kind, std::move(content)});
    }
    return out;
}

/// @brief 渲染 shell 工具结果：剥离标签，<error> 内容红色，长行按列宽折行
Element render_shell_result(const ToolCallNode& t, int width) {
    const auto sections = parse_shell_sections(t.result);
    bool has_tag = false;
    for (const auto& sec : sections)
        if (sec.kind != ShellSectionKind::Plain) {
            has_tag = true;
            break;
        }
    if (!has_tag) return build_markdown(t.result, width);  // 无标签：原行为

    Elements rows;
    for (const auto& sec : sections) {
        if (sec.text.empty()) continue;
        const Color c = (sec.kind == ShellSectionKind::Error) ? Color::RedLight : theme::T::Text;
        std::string cur;
        auto flush = [&]() {
            for (auto [b, e] : wrap_text(cur, width))
                rows.push_back(ftxui::color(c)(ftxui::text(cur.substr(b, e - b))));
            cur.clear();
        };
        for (const char ch : sec.text) {
            if (ch == '\n')
                flush();
            else
                cur.push_back(ch);
        }
        flush();
    }
    return ftxui::vbox(std::move(rows));
}

Element render_tool_result(const ToolCallNode& t, int width) {
    if (t.is_error) return render_shell_result(t, width);
    const std::string fpath = tool_file_path(t.arguments);
    const bool is_read = t.tool_name == "Read" || t.tool_name == "FileRead";
    const bool is_write_or_edit = t.tool_name == "Write" || t.tool_name == "Edit" ||
                                  t.tool_name == "FileWrite" || t.tool_name == "FileEdit";
    if (!is_read && !is_write_or_edit) return render_shell_result(t, width);

    const std::string lang = lang_from_path(fpath);
    const std::string arrow = "\u23bf";  // ⎿
    const std::string indent4 = "    ";
    Elements rows;

    // ---- 分支 A: Write/Edit — 拆分状态文本 + diff ----
    if (is_write_or_edit) {
        size_t pos = 0, line_start = 0, diff_start = std::string::npos;
        while (pos <= t.result.size()) {
            if (pos == t.result.size() || t.result[pos] == '\n') {
                if (pos - line_start >= 4 && t.result.compare(line_start, 4, "--- ") == 0) {
                    diff_start = line_start;
                    break;
                }
                line_start = pos + 1;
            }
            ++pos;
        }
        if (diff_start == std::string::npos) {
            // 无 diff（create 模式 / 无变化）：退回通用渲染
            return build_markdown(t.result, width);
        }

        // 状态文本（Dim 色，⎿ 前缀；对齐 src/tui 分支 A）
        std::string status_text = t.result.substr(0, diff_start);
        while (!status_text.empty() && status_text.back() == '\n') status_text.pop_back();
        std::string line;
        auto flush_status = [&]() {
            if (!line.empty()) {
                rows.push_back(ftxui::hbox({
                    ftxui::text(indent4 + arrow + " "),
                    ftxui::color(theme::T::TextDim)(ftxui::text(line)),
                }));
                line.clear();
            }
        };
        for (const char c : status_text) {
            if (c == '\n')
                flush_status();
            else
                line.push_back(c);
        }
        flush_status();

        // diff 块：Panel 背景 + 上下留白 + │真实行号 + 前景高亮 + +/- 背景色
        std::vector<DiffLine> diff = parse_diff_lines(t.result.substr(diff_start));
        const bool truncated = static_cast<int>(diff.size()) > kMaxToolResultLines;
        if (truncated) diff.resize(kMaxToolResultLines);
        // 每行显示的行号：新增行→新文件行号，删除行→旧文件行号，上下文→新文件行号
        std::vector<int> disp_no;
        disp_no.reserve(diff.size());
        int max_line_num = 0;
        for (const auto& dl : diff) {
            int n = 0;
            if (dl.prefix == DiffPrefix::Add && dl.new_no > 0)
                n = dl.new_no;
            else if (dl.prefix == DiffPrefix::Del && dl.old_no > 0)
                n = dl.old_no;
            else if (dl.prefix == DiffPrefix::Context && dl.new_no > 0)
                n = dl.new_no;
            disp_no.push_back(n);
            max_line_num = std::max(max_line_num, n);
        }
        const int num_width = calc_line_num_width(max_line_num);
        // diff 内容行整块送 tree-sitter 前景高亮，再叠加 +/- 背景色
        std::vector<std::string> diff_contents;
        diff_contents.reserve(diff.size());
        for (const auto& dl : diff) diff_contents.push_back(dl.content);
        const std::vector<Element> hl = highlight_code_block(diff_contents, lang);
        Elements code_rows;
        code_rows.reserve(diff.size());
        for (size_t i = 0; i < diff.size(); ++i) {
            const DiffLine& dl = diff[i];
            Element content = (i < hl.size()) ? hl[i] : highlight_code_line(dl.content, lang);
            if (dl.prefix == DiffPrefix::Add) {
                content = content | ftxui::bgcolor(Color::RGB(0x00, 0x5f, 0x00));
            } else if (dl.prefix == DiffPrefix::Del) {
                content = content | ftxui::bgcolor(Color::RGB(0x5f, 0x00, 0x00));
            }
            code_rows.push_back(ftxui::hbox({
                ftxui::text("  "),
                disp_no[i] > 0 ? line_num_prefix(disp_no[i], num_width) : ftxui::text(" "),
                ftxui::flex(content),
            }));
        }
        if (truncated) {
            code_rows.push_back(ftxui::hbox({
                ftxui::text(indent4 + arrow + " "),
                ftxui::color(theme::T::TextDim)(ftxui::text(
                    std::format("(... truncated, showing first {} lines)", kMaxToolResultLines))),
            }));
        }
        rows.push_back(
            theme::vPad(ftxui::vbox(std::move(code_rows)) | ftxui::bgcolor(theme::T::Panel)));
        return ftxui::vbox(std::move(rows));
    }

    // ---- 分支 B: FileRead — 状态行 + 剥离行号 + 末尾元数据 ----
    rows.push_back(ftxui::hbox({
        ftxui::text(indent4 + arrow + " "),
        ftxui::color(theme::T::TextDim)(ftxui::text("The file " +
                                                    (fpath.empty() ? std::string{"file"} : fpath) +
                                                    " has been read successfully.")),
    }));

    std::vector<std::string> lines;
    {
        std::string cur;
        for (const char c : t.result) {
            if (c == '\n') {
                lines.push_back(std::move(cur));
                cur.clear();
            } else
                cur.push_back(c);
        }
        lines.push_back(std::move(cur));
    }
    // 剥离末尾元数据行（(read lines ...) 及其前的空行）
    std::vector<std::string> meta_lines;
    while (!lines.empty()) {
        const std::string& last = lines.back();
        if (last.empty() || is_fileread_metadata_line(last)) {
            meta_lines.insert(meta_lines.begin(), last);
            lines.pop_back();
        } else {
            break;
        }
    }
    // 截断（代码部分，不含元数据）
    const bool truncated = static_cast<int>(lines.size()) > kMaxToolResultLines;
    if (truncated) lines.resize(kMaxToolResultLines);

    // 剥离行号前缀 + 提取实际行号
    std::vector<int> line_nums;
    line_nums.reserve(lines.size());
    int max_line_num = 0;
    std::vector<std::string> code_lines;
    code_lines.reserve(lines.size());
    for (const auto& l : lines) {
        std::string prefix, code;
        split_fileread_line(l, prefix, code);
        const int n = extract_fileread_line_number(prefix);
        line_nums.push_back(n);
        max_line_num = std::max(max_line_num, n);
        code_lines.push_back(std::move(code));
    }
    const int num_width = calc_line_num_width(max_line_num);
    const std::vector<Element> hl = highlight_code_block(code_lines, lang);
    Elements code_rows;
    code_rows.reserve(code_lines.size());
    for (size_t i = 0; i < code_lines.size(); ++i) {
        Elements row;
        row.push_back(ftxui::text("  "));
        if (line_nums[i] > 0)
            row.push_back(line_num_prefix(line_nums[i], num_width));
        else
            row.push_back(ftxui::text(""));
        row.push_back(
            ftxui::flex((i < hl.size()) ? hl[i] : highlight_code_line(code_lines[i], lang)));
        code_rows.push_back(ftxui::hbox(std::move(row)));
    }
    if (truncated) {
        code_rows.push_back(ftxui::hbox({
            ftxui::text(indent4 + arrow + " "),
            ftxui::color(theme::T::TextDim)(ftxui::text(
                std::format("(... truncated, showing first {} lines)", kMaxToolResultLines))),
        }));
    }
    rows.push_back(
        theme::vPad(ftxui::vbox(std::move(code_rows)) | ftxui::bgcolor(theme::T::Panel)));

    // 元数据行（Dim，不高亮）
    for (const auto& m : meta_lines) {
        if (m.empty()) {
            rows.push_back(ftxui::text(""));
        } else {
            rows.push_back(ftxui::hbox({
                ftxui::text(indent4 + arrow + " "),
                ftxui::color(theme::T::TextDim)(ftxui::text(m)),
            }));
        }
    }
    return ftxui::vbox(std::move(rows));
}

/// @brief 工具结果展开行数估算（与 render_tool_result 布局逐行对齐；A3 单一布局源）
int estimate_shell_result_lines(const ToolCallNode& t,
                                int width);  // 定义见后（依赖 estimate_markdown_height）

int estimate_tool_result_lines(const ToolCallNode& t, int width) {
    const std::string fpath = tool_file_path(t.arguments);
    const bool is_read = t.tool_name == "Read" || t.tool_name == "FileRead";
    const bool is_write_or_edit = t.tool_name == "Write" || t.tool_name == "Edit" ||
                                  t.tool_name == "FileWrite" || t.tool_name == "FileEdit";
    if (t.is_error || (!is_read && !is_write_or_edit)) return estimate_shell_result_lines(t, width);

    if (is_write_or_edit) {
        size_t pos = 0, line_start = 0, diff_start = std::string::npos;
        while (pos <= t.result.size()) {
            if (pos == t.result.size() || t.result[pos] == '\n') {
                if (pos - line_start >= 4 && t.result.compare(line_start, 4, "--- ") == 0) {
                    diff_start = line_start;
                    break;
                }
                line_start = pos + 1;
            }
            ++pos;
        }
        if (diff_start == std::string::npos)
            return std::max(1, estimate_markdown_height(t.result, width));

        int status_lines = 0;
        bool in_line = false;
        for (size_t i = 0; i < diff_start; ++i) {
            if (t.result[i] == '\n') {
                if (in_line) ++status_lines;
                in_line = false;
            } else
                in_line = true;
        }
        if (in_line) ++status_lines;

        const int diff_lines = static_cast<int>(
            parse_diff_lines(std::string_view(t.result).substr(diff_start)).size());
        int h = status_lines + std::min(diff_lines, kMaxToolResultLines) + 2;  // vPad
        if (diff_lines > kMaxToolResultLines) ++h;                             // 截断提示行
        return std::max(1, h);
    }

    // Read：状态行 1 + 代码行（截断）+ vPad 2 + 元数据 + 截断提示
    int code_lines = 0, meta_lines = 0;
    {
        std::vector<std::string> lines;
        std::string cur;
        for (const char c : t.result) {
            if (c == '\n') {
                lines.push_back(std::move(cur));
                cur.clear();
            } else
                cur.push_back(c);
        }
        lines.push_back(std::move(cur));
        while (!lines.empty()) {
            const std::string& last = lines.back();
            if (last.empty() || is_fileread_metadata_line(last)) {
                ++meta_lines;
                lines.pop_back();
            } else {
                break;
            }
        }
        code_lines = static_cast<int>(lines.size());
    }
    int h = 1 + std::min(code_lines, kMaxToolResultLines) + 2 + meta_lines;
    if (code_lines > kMaxToolResultLines) ++h;  // 截断提示行
    return std::max(1, h);
}

int estimate_markdown_height(std::string_view text, int width, bool compact) {
    // build_markdown 入口对空输入返回 text("")（FTXUI 空文本 min_y=1）
    if (text.empty()) return 1;
    return count_markdown_lines(split_lines(text), width, compact);
}

/// @brief shell 结果行数估算：与 render_shell_result 逐行对齐（A3 单一布局源）
///        含标签时按「先按 \n 切逻辑行、再 wrap_text 折行」计数；无标签退回 markdown 估算
int estimate_shell_result_lines(const ToolCallNode& t, int width) {
    const auto sections = parse_shell_sections(t.result);
    bool has_tag = false;
    for (const auto& sec : sections)
        if (sec.kind != ShellSectionKind::Plain) {
            has_tag = true;
            break;
        }
    if (!has_tag) return std::max(1, estimate_markdown_height(t.result, width));

    int lines = 0;
    for (const auto& sec : sections) {
        if (sec.text.empty()) continue;
        std::string cur;
        auto flush = [&]() {
            for (auto [b, e] : wrap_text(cur, width))
                if (e > b) ++lines;
            cur.clear();
        };
        for (const char ch : sec.text) {
            if (ch == '\n')
                flush();
            else
                cur.push_back(ch);
        }
        flush();
    }
    return std::max(1, lines);
}

/// @brief 展开态标题换行行数估算（与 render_tool_card 展开态标题逐行对齐）
int estimate_tool_title_lines(const ToolCallNode& t, int width) {
    const std::string subject = tool_card_subject(t.arguments);
    if (subject.empty()) return 0;
    int lines = 0;
    for (auto [b, e] : wrap_text(subject, card_content_width(width) - 4))
        if (e > b) ++lines;
    return lines;
}

/// 是否为"手动调用技能"注入的合成 Skill 卡片节点
/// （空正文 + 唯一 Skill 工具卡且 call_id 为空；真实模型调 Skill 工具的卡 call_id 非空）
/// 此类卡片上下零间距（紧贴相邻消息/卡片），不参与通用首/末卡空行。
bool is_synthetic_skill_card(const MessageNode& m) {
    return m.tool_calls.size() == 1 && m.tool_calls[0].tool_name == "Skill" &&
           m.tool_calls[0].call_id.empty() && m.text.empty();
}

int estimate_message_height(const MessageNode& msg, int width) {
    // 与 build_message 的视觉结构逐行对齐（A3 单一布局源）：
    // - 用户块：markdown 内容（上下留白由转录音区统一追加，自身不记）
    // - 思考/工具卡：圆角边框 2 行 + 头行 1 行 + 展开内容
    // - 流式游标 1 行；正文行恒 ≥1 行（左侧缩进 text("  ") 占 1 行）
    // 正文行数随 width 变化（按显示列宽折行）→ 传入与 build_message 相同的宽度。
    auto content_lines = [](std::string_view t, int w) {
        return std::max(1, estimate_markdown_height(t, w));
    };
    if (msg.role == MsgRole::User) {
        int h = 0;
        // 内容 + 上下各空一行（与 build_message 的 UserMessageBox 布局对齐）
        if (!msg.text.empty() || msg.streaming)
            h += std::max(1, estimate_markdown_height(msg.text, user_body_width(width),
                                                      /*compact=*/true)) +
                 2;
        return h;
    }

    int h = 0;
    if (msg.role == MsgRole::Error) ++h;  // "✖ 错误" 头
    if (msg.reasoned && !msg.reasoning.empty()) {
        h += 3;  // 边框 2 行 + 头行 1 行
        if (msg.reasoning_expanded) h += content_lines(msg.reasoning, card_content_width(width));
    }
    if (!msg.text.empty() || msg.streaming) h += content_lines(msg.text, message_body_width(width));
    for (const auto& t : msg.tool_calls) {
        h += 3;  // 边框 2 行 + 头行 1 行
        if (t.done && t.expanded) {
            const int title_lines = estimate_tool_title_lines(t, width);
            h += title_lines + (title_lines > 0 ? 1 : 0)  // 标题与输出之间的空行
                 + estimate_tool_result_lines(t, card_content_width(width)) +
                 (tool_file_path(t.arguments).empty() ? 0 : 1);
        }
    }
    // 卡片间距：文本与卡片直接相邻；仅首卡（无正文在前）前空 1 行 + 末卡后无正文时空 1 行
    // （与 build_message 交错循环对齐）。合成 Skill 卡上下零间距，跳过该间距。
    if (!msg.tool_calls.empty() && !is_synthetic_skill_card(msg)) {
        const std::size_t pos0 = std::min(msg.tool_calls[0].text_pos, msg.text.size());
        if (pos0 == 0) ++h;  // 首卡无正文在前 → 前空一行
        std::size_t max_pos = 0;
        for (const auto& t : msg.tool_calls)
            max_pos = std::max(max_pos, std::min(t.text_pos, msg.text.size()));
        if (max_pos >= msg.text.size()) ++h;  // 末卡后无正文 → 后空一行
    }
    if (msg.streaming) ++h;  // 流式游标
    // 操作按钮栏（复制/重试）：仅模型回复正文（非 notice、有正文），思考/工具调用专用消息不显示
    if (msg.role == MsgRole::Assistant && msg.sealed && !msg.notice && !trim_copy(msg.text).empty())
        ++h;
    return h;
}

Element build_message(const MessageNode& msg, int width, std::size_t anim_frame,
                      std::deque<CardHit>* card_hits) {
    // 用户消息：深色背景块 + 左边框线，无角色头，内容左缩进 2 格。
    // 内容上下各空一行，使背景块/左边框上下留白，消息间隔视觉更舒展；
    // 转录音区每条消息后仍统一追加 +1 行，整体上/下间隔保持对称。
    if (msg.role == MsgRole::User) {
        Elements body;
        if (!msg.text.empty() || msg.streaming) {
            // 可用宽 = width - 外层缩进 2 - 边框 1 - 内层缩进 2
            // compact：普通段落每物理行占 1 行，块高即文本行数，不产生段落尾随空行
            body.push_back(ftxui::text(" "));  // 上方空行
            body.push_back(ftxui::hbox(
                {ftxui::text("  "),
                 ftxui::flex(build_markdown(msg.text, user_body_width(width), /*compact=*/true))}));
            body.push_back(ftxui::text(" "));  // 下方空行
        }
        return std::make_shared<UserMessageBox>(ftxui::vbox(std::move(body)));
    }

    Elements rows;

    if (msg.role == MsgRole::Error) {
        rows.push_back(ftxui::hbox({
            ftxui::text(std::string(str::kErrorHeader)) | ftxui::color(Color::RedLight) |
                ftxui::bold,
            ftxui::separatorEmpty(),
        }));
    }

    // 思考折叠（reasoning）：圆角卡片，头行可点击展开/收起
    if (msg.reasoned && !msg.reasoning.empty()) {
        Element thinking_header;
        if (!msg.sealed) {
            // 运行中：Braille 旋转动画 + 橙黄渐变（src/tui 同款）
            const char* spin = kSpinnerFrames[anim_frame % kSpinnerFrameCount];
            Color c = spinner_color(anim_frame);
            thinking_header = ftxui::hbox({
                ftxui::text(spin) | ftxui::color(c),
                ftxui::text(std::string(str::kThinkingRunning)) | ftxui::color(c),
            });
        } else {
            std::string sec = std::format("{:.1f}s", msg.reasoning_ms / 1000.0);
            // Nerd Font（不支持时 ASCII 降级）：灯泡=思考，chevron=折叠指示
            const std::string chevron(msg.reasoning_expanded ? theme::icon_chevron_down()
                                                             : theme::icon_chevron_right());
            thinking_header = ftxui::hbox({
                ftxui::text(std::string(theme::icon_think()) + std::string(str::kThinkingLabel)),
                ftxui::text(sec),
                ftxui::text("  "),
                ftxui::text(chevron),
            });
        }
        Elements card_rows;
        card_rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            thinking_header,
        }));
        if (msg.reasoning_expanded) {
            card_rows.push_back(ftxui::hbox(
                {ftxui::text("    "), ftxui::flex(ftxui::color(theme::T::Text)(build_markdown(
                                          msg.reasoning, card_content_width(width))))}));
        }
        auto card = ftxui::borderStyled(BorderStyle::ROUNDED,
                                        msg.sealed ? kCardBorder : spinner_color(anim_frame))(
            ftxui::vbox(std::move(card_rows)));
        if (card_hits) {
            card_hits->push_back(CardHit{});
            card_hits->back().tool_idx = -1;
            card = card | ftxui::reflect(card_hits->back().box);
        }
        rows.push_back(std::move(card));
    }

    // 工具块：圆角卡片，头行可点击展开/收起
    auto render_tool_card = [&](std::size_t ti, const ToolCallNode& t) -> Element {
        // Skill 工具：独立 Skills 卡片（技能魔杖图标 + "Skills：<技能名>"），
        // 不再作为普通工具卡回填到所在消息，让技能加载动作清晰可辨。
        const bool is_skill = (t.tool_name == "Skill");
        const std::string skill_subject =
            is_skill ? ("Skills：" + skill_name_from_args(t.arguments)) : std::string{};
        // skill 的状态文案即含完整主题，标题行不必再重复显示 subject
        const std::string subject = is_skill ? std::string{} : tool_card_subject(t.arguments);

        std::string status;
        Color c = Color::YellowLight;
        if (t.running) {
            // 运行中：Braille 旋转动画 + 橙黄渐变（src/tui 同款）
            status = std::string(kSpinnerFrames[anim_frame % kSpinnerFrameCount]) + " " +
                     (is_skill ? skill_subject : std::string(t.tool_name));
            c = spinner_color(anim_frame);
        } else if (t.is_error) {
            status = "✖ " + (is_skill ? skill_subject : std::string(t.tool_name));
            c = Color::RedLight;
        } else {
            status = "✓ " + (is_skill ? skill_subject : std::string(t.tool_name));
            c = Color::GreenLight;
        }

        Elements card_rows;
        const bool expanded = t.done && t.expanded;
        // 头行：图标 + 状态；折叠态标题单行截断跟在状态后
        Elements header;
        header.push_back(ftxui::text("  "));
        // 工具图标（Nerd Font 不支持时降级为空/S；Skill 用技能魔杖图标）
        header.push_back(
            ftxui::text(std::string(is_skill ? theme::icon_skill() : theme::icon_tool()) + " "));
        header.push_back(ftxui::color(c)(ftxui::text(status)));
        if (!expanded && !subject.empty()) {
            // 为图标/状态预留边界（含 2 空格空隙），长标题尽量铺满整行、短标题自然收缩
            const int subject_max = std::max(12, card_content_width(width) - 14);
            header.push_back(ftxui::hbox({
                ftxui::text(" "),
                ftxui::color(theme::T::TextDim)(
                    ftxui::text(truncate_display(subject, subject_max))),
                ftxui::text("  "),  // 省略号后 2 空格空隙，再靠边框
            }));
        }
        card_rows.push_back(ftxui::hbox(std::move(header)));
        if (expanded && !subject.empty()) {
            // 展开态：标题不截断，超出宽度自动换行（独立行，4 空格缩进）
            Elements subj_lines;
            for (auto [b, e] : wrap_text(subject, card_content_width(width) - 4)) {
                subj_lines.push_back(ftxui::hbox({
                    ftxui::text("    "),
                    ftxui::color(theme::T::TextDim)(ftxui::text(subject.substr(b, e - b))),
                }));
            }
            card_rows.push_back(ftxui::vbox(std::move(subj_lines)));
            card_rows.push_back(ftxui::text(" "));  // 标题与输出之间空一行
        }
        if (expanded) {
            Elements content;
            const std::string fpath = tool_file_path(t.arguments);
            if (!fpath.empty()) {
                content.push_back(ftxui::hbox({
                    ftxui::text("    "),
                    ftxui::flex(ftxui::color(theme::T::TextDim)(ftxui::text(fpath))),
                }));
            }
            content.push_back(
                ftxui::hbox({ftxui::text("    "),
                             ftxui::flex(render_tool_result(t, card_content_width(width)))}));
            card_rows.push_back(ftxui::vbox(std::move(content)));
        }
        auto card = ftxui::borderStyled(
            BorderStyle::ROUNDED,
            t.running ? spinner_color(anim_frame) : kCardBorder)(ftxui::vbox(std::move(card_rows)));
        if (card_hits) {
            card_hits->push_back(CardHit{});
            card_hits->back().tool_idx = static_cast<int>(ti);
            card = card | ftxui::reflect(card_hits->back().box);
        }
        // 卡片本身不包裹 vPad：间距由交错循环统一插入单行分隔，
        // 避免相邻卡片各自上下留白叠加成双倍间距
        // 折叠态卡片收缩到内容宽度（消除标题后的尾部空白）；展开态需撑满以
        // 容纳正文/工具结果内容。行数不变（折叠恒 1 行头），高度估算无需调整。
        if (t.done && t.expanded) return card;
        return ftxui::hbox({card, ftxui::flex(ftxui::text(""))});
    };

    // 正文 + 工具卡交错：模型在回复中途调用工具时，卡片显示在对应正文之后
    // （而非统一堆到末尾）。工具卡按 text_pos（正文插入点）排序后，将正文
    // 按插入点切分为若干段，与卡片交替渲染。
    std::vector<std::size_t> tool_order(msg.tool_calls.size());
    for (std::size_t i = 0; i < tool_order.size(); ++i) tool_order[i] = i;
    std::stable_sort(tool_order.begin(), tool_order.end(), [&](std::size_t a, std::size_t b) {
        return msg.tool_calls[a].text_pos < msg.tool_calls[b].text_pos;
    });

    if (tool_order.empty()) {
        // 无工具卡：正文整体渲染
        if (!msg.text.empty() || msg.streaming) {
            rows.push_back(
                ftxui::hbox({ftxui::text("  "),
                             ftxui::flex(build_markdown(msg.text, message_body_width(width)))}));
        }
    } else {
        std::size_t cursor = 0;
        bool first_card = true;
        for (const std::size_t ti : tool_order) {
            const auto& t = msg.tool_calls[ti];
            const std::size_t pos = std::min(t.text_pos, msg.text.size());
            if (pos > cursor) {
                rows.push_back(ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::flex(build_markdown(msg.text.substr(cursor, pos - cursor),
                                               message_body_width(width))),
                }));
                cursor = pos;
                first_card = false;
            }
            // 文本与卡片直接相邻；仅首卡（无正文在前）前空一行，与思考卡/消息顶分隔
            // （合成 Skill 卡上下零间距）
            if (first_card && !is_synthetic_skill_card(msg)) rows.push_back(ftxui::text(" "));
            rows.push_back(render_tool_card(ti, t));
            first_card = false;
        }
        if (cursor < msg.text.size()) {
            // 末卡与后续正文直接相邻（不插空行）
            rows.push_back(ftxui::hbox({
                ftxui::text("  "),
                ftxui::flex(build_markdown(msg.text.substr(cursor), message_body_width(width))),
            }));
        } else {
            // 末卡后无正文：空一行（与流式游标/消息分隔）
            // （合成 Skill 卡上下零间距，不插末行空行）
            if (!is_synthetic_skill_card(msg)) rows.push_back(ftxui::text(" "));
        }
    }

    // 流式游标
    if (msg.streaming) {
        rows.push_back(
            ftxui::hbox({ftxui::text("  "), ftxui::text("▋") | ftxui::color(theme::T::TextFaint)}));
    }

    // 消息操作按钮栏（复制 / 重试）：仅模型回复正文（非 notice），思考/工具调用专用与通知文本不显示
    if (msg.role == MsgRole::Assistant && msg.sealed && !msg.notice &&
        !trim_copy(msg.text).empty()) {
        auto make_btn = [&](int kind, std::string_view icon, std::string_view label) {
            Element e = ftxui::color(theme::T::TextFaint)(
                ftxui::text(std::string(icon) + " " + std::string(label)));
            if (card_hits) {
                card_hits->push_back(CardHit{});
                card_hits->back().tool_idx = -1;
                card_hits->back().button = kind;
                e = e | ftxui::reflect(card_hits->back().box);
            }
            return e;
        };
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            make_btn(0, theme::icon_copy(), str::kMsgCopy),
            ftxui::text("   "),
            make_btn(1, theme::icon_retry(), str::kMsgRetry),
            ftxui::flex(ftxui::text("")),
        }));
    }

    return ftxui::vbox(std::move(rows));
}

}  // namespace ftxtui