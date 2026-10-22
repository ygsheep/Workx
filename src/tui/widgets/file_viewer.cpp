#include "widgets/file_viewer.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "core/utils/line_diff.h"
#include "render/code_card.h"
#include "render/image_view.h"
#include "render/syntax_highlight.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 可视行数估算：侧栏内容区高度 − 路径栏/分隔线/状态栏
int visible_line_count() {
    const int term_h = ftxui::Terminal::Size().dimy;
    return std::max(1, term_h - 7);
}

/// @brief UTF-8 rune 字节长度（1~4；无效则回退 1）
std::size_t rune_byte_len(std::string_view s) {
    const unsigned char c = static_cast<unsigned char>(s[0]);
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/// @brief 折行后的一段（保留在原逻辑行中的字节区间，供 span 着色定位）
struct WrappedSeg {
    std::string text;    ///< 本段文本
    uint32_t start = 0;  ///< 在原逻辑行中的字节起点（含）
    uint32_t end = 0;    ///< 在原逻辑行中的字节终点（不含）
};

/// @brief 按显示宽度折行（UTF-8 安全、rune 边界分段）
/// @return 每段为一行（空行返回单空段）
std::vector<WrappedSeg> wrap_line(std::string_view line, int width) {
    std::vector<WrappedSeg> out;
    const std::size_t n = line.size();
    if (n == 0) {
        out.push_back({std::string(), 0, 0});
        return out;
    }
    if (width < 1) width = 1;
    std::string seg;
    seg.reserve(n);
    int seg_w = 0;
    uint32_t seg_start = 0;
    std::size_t i = 0;
    while (i < n) {
        const std::size_t rune_len = std::min(rune_byte_len(line.substr(i)), n - i);
        const std::string_view rune = line.substr(i, rune_len);
        const std::size_t rune_i = i;
        i += rune_len;
        const int rw = ftxui::string_width(rune);
        if (seg_w + rw > width && !seg.empty()) {
            out.push_back({std::move(seg), seg_start, static_cast<uint32_t>(rune_i)});
            seg.clear();
            seg_w = 0;
            seg_start = static_cast<uint32_t>(rune_i);
        }
        seg += rune;
        seg_w += rw;
    }
    out.push_back({std::move(seg), seg_start, static_cast<uint32_t>(n)});
    return out;
}

/// @brief 扁平可视行（折行后；scroll 以该序列为索引）
struct FlatRow {
    int disp_no = 0;          ///< 展示行号（0=续行/无行号）
    std::string text;         ///< 本行文本
    Color bg = Color::Black;  ///< 背景色（Black=无）
    uint32_t seg_start = 0;  ///< 本段在原逻辑行中的字节起点（语法高亮定位用）
    uint32_t seg_end = 0;    ///< 本段在原逻辑行中的字节终点
    int line_idx = 0;        ///< 逻辑行索引（span 着色取整行文本用）
    const std::vector<HighlightSpan>* spans = nullptr;  ///< 该逻辑行 span（可空）
};

/// @brief 行内内容可用宽度（扣除前缀 2 空格 + │行号列 + 1 空格）
int row_content_width(int avail_width, int num_width) {
    return std::max(1, avail_width - num_width - 4);
}

}  // namespace

/// @brief 构建扁平可视行（含折行）。avail_width<=0 时不折行。
/// @param[out] num_w 行号列宽度
/// @param[out] line_spans 逐逻辑行语法高亮 span（可为空，仅统计行数时跳过高亮）。
///             FlatRow::spans 指向该数组，调用方须保证其生命周期覆盖返回的 flat。
static std::vector<FlatRow> build_flat_rows(const FileViewState& file, int avail_width, int* num_w,
                                            std::vector<std::vector<HighlightSpan>>* line_spans) {
    std::vector<FlatRow> flat;
    const bool is_diff = codecard::looks_like_diff(file.lines);

    if (is_diff) {
        std::string text;
        {
            std::size_t total = 0;
            for (const auto& l : file.lines) total += l.size() + 1;
            text.reserve(total);
            for (const auto& l : file.lines) {
                text += l;
                text.push_back('\n');
            }
        }
        const std::vector<codecard::DiffLine> diff = codecard::parse_diff_lines(text);
        int max_no = 0;
        for (const auto& dl : diff) max_no = std::max(max_no, std::max(dl.old_no, dl.new_no));
        *num_w = codecard::calc_line_num_width(max_no);
        const int cw = avail_width > 0 ? row_content_width(avail_width, *num_w) : (1 << 20);
        for (const codecard::DiffLine& dl : diff) {
            const int disp_no = (dl.prefix == codecard::DiffPrefix::Add)   ? dl.new_no
                                : (dl.prefix == codecard::DiffPrefix::Del) ? dl.old_no
                                                                           : dl.new_no;
            const Color bg = codecard::diff_row_background(dl.prefix);
            const auto segs = wrap_line(dl.content, cw);
            for (std::size_t k = 0; k < segs.size(); ++k) {
                const WrappedSeg& s = segs[k];
                flat.push_back({k == 0 ? disp_no : 0, s.text, bg, s.start, s.end, 0, nullptr});
            }
        }
        return flat;
    }

    // ---- 普通代码文件 ----
    *num_w = codecard::calc_line_num_width(static_cast<int>(file.lines.size()));
    const int cw = avail_width > 0 ? row_content_width(avail_width, *num_w) : (1 << 20);

    std::map<int, agent::DiffKind> diff_mark;
    for (const auto& ch : file.changes) {
        if (ch.new_start <= 0) continue;
        for (const auto& d : ch.diff)
            if (d.kind != agent::DiffKind::Equal) diff_mark[ch.new_start + d.line_no - 1] = d.kind;
    }
    const Color mod_bg = codecard::diff_row_background(codecard::DiffPrefix::Add);

    // 整文件 tree-sitter 高亮（逐逻辑行 span）；空 = 无 grammar，按行回退关键字
    if (line_spans) *line_spans = highlight_block_spans(file.lines, file.lang);
    const bool has_spans = line_spans && !line_spans->empty();

    flat.reserve(file.lines.size());
    for (std::size_t i = 0; i < file.lines.size(); ++i) {
        const int disp_no = static_cast<int>(i + 1);
        const Color bg = diff_mark.count(disp_no) != 0 ? mod_bg : Color::Black;
        const auto segs = wrap_line(file.lines[i], cw);
        for (std::size_t k = 0; k < segs.size(); ++k) {
            const WrappedSeg& s = segs[k];
            flat.push_back({k == 0 ? disp_no : 0, s.text, bg, s.start, s.end, static_cast<int>(i),
                            has_spans ? &(*line_spans)[i] : nullptr});
        }
    }
    return flat;
}

namespace {

/// @brief 单行渲染：2 空格 + 行号/空列 + 语法高亮内容（自动折行续行对齐）
Element row_element(const FlatRow& r, int num_w, const FileViewState& file) {
    Element content;
    if (r.spans && !r.spans->empty() && r.seg_end > r.seg_start && r.line_idx >= 0 &&
        r.line_idx < static_cast<int>(file.lines.size())) {
        content = render_spans_range(file.lines[r.line_idx], r.seg_start, r.seg_end, *r.spans);
    } else {
        content = highlight_code_line(r.text, file.lang);
    }
    if (r.bg != Color::Black) content = content | ftxui::bgcolor(r.bg);
    Element prefix;
    if (r.disp_no > 0) {
        if (r.bg != Color::Black) {
            // 内联 diff 标记行：行号前加 + 标记，标示该行为新增/修改（未变更行无标记）
            const std::string num_str = std::to_string(r.disp_no);
            const int pad = std::max(0, num_w - static_cast<int>(num_str.size()));
            prefix = ftxui::color(theme::T::TextDim)(
                ftxui::text(std::string("\u2502") + "+" + std::string(pad, ' ') + num_str + " "));
        } else {
            prefix = codecard::line_num_prefix(r.disp_no, num_w);
        }
    } else {
        // 续行：保留 │ 竖线分隔（无数字、同宽对齐），避免换行后竖线消失
        prefix = ftxui::text(std::string("\u2502") +
                             std::string(static_cast<std::size_t>(num_w + 1), ' '));
    }
    return ftxui::hbox({
        ftxui::text("  "),
        prefix,
        ftxui::flex(std::move(content)),
    });
}

}  // namespace

Element build_file_viewer(const FileViewState& file, int avail_width, int avail_height) {
    if (file.path.empty()) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kTabFilesEmpty)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }

    // —— 图片预览模式（/view 或项目树点击图片）——
    if (file.image) {
        const int avail_w =
            avail_width > 0 ? avail_width : std::max(40, ftxui::Terminal::Size().dimx - 4);
        const int avail_h = avail_height > 0 ? avail_height : visible_line_count();
        const int rows = image_view_rows(*file.image, avail_w, avail_h);
        const int scroll = std::clamp(file.scroll, 0, std::max(0, rows - avail_h));

        const std::string meta =
            std::to_string(file.image->width) + "x" + std::to_string(file.image->height);
        const std::string hint =
            rows > avail_h ? std::string(str::kViewScrollHint) : std::string(str::kViewImageHint);

        return ftxui::vbox({
            ftxui::hbox({
                ftxui::text(" "),
                ftxui::text(file.path) | ftxui::color(theme::T::Text),
                ftxui::flex(ftxui::text("")),
                ftxui::text(meta) | ftxui::color(theme::T::TextFaint),
                ftxui::text(" "),
            }),
            ftxui::separator() | ftxui::color(theme::T::TextFaint),
            build_image_content(*file.image, avail_w, avail_h, scroll) |
                ftxui::bgcolor(theme::T::Panel),
            ftxui::separator() | ftxui::color(theme::T::TextFaint),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(hint) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }

    int num_w = 1;
    std::vector<std::vector<HighlightSpan>> line_spans;
    std::vector<FlatRow> flat = build_flat_rows(file, avail_width, &num_w, &line_spans);
    const int visible = avail_height > 0 ? avail_height : visible_line_count();
    const int scroll =
        std::clamp(file.scroll, 0, std::max(0, static_cast<int>(flat.size()) - visible));

    const bool is_diff = codecard::looks_like_diff(file.lines);
    Elements line_els;
    line_els.reserve(static_cast<std::size_t>(visible));
    for (int i = 0; i < visible; ++i) {
        const std::size_t idx = static_cast<std::size_t>(scroll + i);
        if (idx >= flat.size()) {
            line_els.push_back(ftxui::text(""));
            continue;
        }
        line_els.push_back(row_element(flat[idx], num_w, file));
    }

    std::string meta = std::to_string(file.lines.size()) + std::string(str::kViewLineSuffix);
    if (!file.lang.empty()) meta += std::string(str::kViewLangSep) + file.lang;

    return ftxui::vbox({
        ftxui::hbox({
            ftxui::text(" "),
            ftxui::text(file.path) | ftxui::color(theme::T::Text),
            ftxui::flex(ftxui::text("")),
            ftxui::text(meta) | ftxui::color(theme::T::TextFaint),
            ftxui::text(" "),
        }),
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        ftxui::vbox(std::move(line_els)) | ftxui::yflex | ftxui::bgcolor(theme::T::Panel),
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(is_diff ? std::string(str::kViewDiffHint)
                                : std::string(str::kViewScrollHint)) |
                ftxui::color(theme::T::TextFaint),
        }),
    });
}

/// @brief 文件查看 tab 可聚焦组件：↑↓/PgUp/PgDn/滚轮滚动（含自动折行后行数）
namespace {
class FileViewer : public ftxui::ComponentBase {
   public:
    explicit FileViewer(FileViewState* file) : m_file(file) {}

    bool OnEvent(ftxui::Event event) override {
        if (!m_file || m_file->path.empty()) return false;
        const int avail = m_box.IsEmpty() ? 0 : (m_box.x_max - m_box.x_min + 1);
        const int avail_h = m_box.IsEmpty() ? 0 : (m_box.y_max - m_box.y_min + 1);
        const int visible = avail_h > 0 ? avail_h : visible_line_count();

        // 滚动上限：图片按缩放后行数，文本按折行后扁平行数
        int max_scroll = 0;
        if (m_file->image) {
            const int w = avail > 0 ? avail : std::max(40, ftxui::Terminal::Size().dimx - 4);
            max_scroll = std::max(0, image_view_rows(*m_file->image, w, visible) - visible);
        } else {
            int num_w = 1;
            const std::size_t flat_size = build_flat_rows(*m_file, avail, &num_w, nullptr).size();
            max_scroll = std::max(0, static_cast<int>(flat_size) - visible);
        }

        if (event == ftxui::Event::ArrowUp) {
            m_file->scroll = std::max(0, m_file->scroll - 1);
            return true;
        }
        if (event == ftxui::Event::ArrowDown) {
            m_file->scroll = std::min(max_scroll, m_file->scroll + 1);
            return true;
        }
        if (event == ftxui::Event::PageUp) {
            m_file->scroll = std::max(0, m_file->scroll - visible);
            return true;
        }
        if (event == ftxui::Event::PageDown) {
            m_file->scroll = std::min(max_scroll, m_file->scroll + visible);
            return true;
        }
        // 滚轮不依赖 m_box 命中：App 侧已确认光标落在文件区 box 内才转发到组件，
        // 这里仅按按钮处理，避免组件内部 reflect box 与 App 侧 box 节奏不一致导致假阴性。
        if (event.is_mouse()) {
            if (event.mouse().button == ftxui::Mouse::WheelUp) {
                m_file->scroll = std::max(0, m_file->scroll - 3);
                return true;
            }
            if (event.mouse().button == ftxui::Mouse::WheelDown) {
                m_file->scroll = std::min(max_scroll, m_file->scroll + 3);
                return true;
            }
        }
        return false;
    }

    Element OnRender() override {
        if (!m_file) return ftxui::emptyElement();
        const int avail = m_box.IsEmpty() ? 0 : (m_box.x_max - m_box.x_min + 1);
        const int avail_h = m_box.IsEmpty() ? 0 : (m_box.y_max - m_box.y_min + 1);
        // 图片模式需传入实际可视高度以做缩放与滚动钳制；文本模式保持原调用
        if (m_file->image)
            return build_file_viewer(*m_file, avail, avail_h) | ftxui::reflect(m_box);
        return build_file_viewer(*m_file, avail) | ftxui::reflect(m_box);
    }

   private:
    FileViewState* m_file;
    ftxui::Box m_box;  ///< 文件查看器渲染区域（滚轮命中 + 可用宽度）
};

}  // namespace

ftxui::Component make_file_viewer(FileViewState* file) { return ftxui::Make<FileViewer>(file); }

}  // namespace ftxtui