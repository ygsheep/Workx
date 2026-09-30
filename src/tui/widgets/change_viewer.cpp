#include "widgets/change_viewer.h"

#include <algorithm>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include "core/utils/line_diff.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

namespace {

/// @brief 文件头：路径 + N 处修改
Element file_header(const std::string& path, int count) {
    return ftxui::hbox({
        ftxui::text(" "),
        ftxui::text(path) | ftxui::color(theme::T::Text),
        ftxui::flex(ftxui::text("")),
        ftxui::text(std::to_string(count) + std::string(str::kChangeCountSuffix)) |
            ftxui::color(theme::T::TextFaint),
        ftxui::text(" "),
    });
}

/// @brief 修改点行：`❯ ⚑ 目的`（选中）/ `  ⚑ 目的`（未选中）
Element change_point_line(const FileChange& ch, bool selected) {
    const std::string marker = selected ? std::string(str::kAskCursor) : "  ";
    return ftxui::hbox({
        ftxui::text(marker) | ftxui::color(selected ? theme::T::Accent : theme::T::TextFaint),
        ftxui::text(std::string(str::kChangeIcon)) | ftxui::color(theme::T::Accent),
        ftxui::text(ch.purpose.empty() ? std::string(str::kChangeNoPurpose) : ch.purpose) |
            ftxui::color(selected ? theme::T::Text : theme::T::TextDim),
        ftxui::flex(ftxui::text("")),
    });
}

/// @brief 目的行：默认一行摘要；e 展开完整 reasoning 多行
Element purpose_lines(const FileChange& ch, bool expanded) {
    Elements rows;
    if (!expanded) {
        rows.push_back(ftxui::hbox({
            ftxui::text("   "),
            ftxui::text(std::string(str::kChangePurposeLabel)) | ftxui::color(theme::T::TextFaint),
            ftxui::text(ch.purpose) | ftxui::color(theme::T::TextDim),
        }));
        return ftxui::vbox(std::move(rows));
    }
    const std::string& reasoning = ch.reasoning.empty() ? ch.purpose : ch.reasoning;
    std::string cur;
    for (const char c : reasoning) {
        if (c == '\n') {
            rows.push_back(ftxui::hbox({
                ftxui::text("   "),
                ftxui::text(cur) | ftxui::color(theme::T::TextDim),
            }));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty())
        rows.push_back(ftxui::hbox({
            ftxui::text("   "),
            ftxui::text(cur) | ftxui::color(theme::T::TextDim),
        }));
    return ftxui::vbox(std::move(rows));
}

/// @brief hunk 行：`+行号 内容`（Insert/Modify 绿色）
Element hunk_line(const agent::DiffLine& dl) {
    return ftxui::hbox({
        ftxui::text("   "),
        ftxui::text("+") | ftxui::color(theme::T::DiffAdd),
        ftxui::text(std::to_string(dl.line_no)) | ftxui::color(theme::T::DiffAdd),
        ftxui::text("  "),
        ftxui::text(dl.text) | ftxui::color(theme::T::DiffAdd),
    });
}

}  // namespace

Element build_change_viewer(const ChangeViewState& changes, std::deque<ChangeHit>* hits) {
    if (changes.changes.empty()) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kTabChangesEmpty)) | ftxui::color(theme::T::TextFaint),
            }),
        });
    }

    Elements rows;
    std::string cur_file;
    const int sel = changes.selected;
    for (std::size_t i = 0; i < changes.changes.size(); ++i) {
        const auto& ch = changes.changes[i];
        // 文件头：文件变化时输出（含该文件修改计数）
        if (ch.file_path != cur_file) {
            cur_file = ch.file_path;
            int count = 0;
            for (std::size_t j = i;
                 j < changes.changes.size() && changes.changes[j].file_path == cur_file; ++j)
                ++count;
            rows.push_back(file_header(cur_file, count));
        }
        const bool selected = (static_cast<int>(i) == sel);
        Element line = change_point_line(ch, selected);
        if (hits) {
            hits->push_back(ChangeHit{static_cast<int>(i), {}});
            line = line | ftxui::reflect(hits->back().box);
        }
        rows.push_back(std::move(line));
        // 选中项：目的 + hunk
        if (selected) {
            rows.push_back(purpose_lines(ch, changes.purpose_expanded));
            for (const auto& dl : ch.diff) {
                if (dl.kind == agent::DiffKind::Equal) continue;
                rows.push_back(hunk_line(dl));
            }
        }
    }

    rows.push_back(ftxui::separator() | ftxui::color(theme::T::TextFaint));
    rows.push_back(ftxui::hbox({
        ftxui::text("  "),
        ftxui::text(std::string(str::kChangeHint)) | ftxui::color(theme::T::TextFaint),
    }));

    return ftxui::vbox(std::move(rows)) | ftxui::yflex;
}

namespace {

class ChangeViewer : public ftxui::ComponentBase {
   public:
    ChangeViewer(ChangeViewState* changes, std::function<void()> on_jump)
        : m_changes(changes), m_on_jump(std::move(on_jump)) {}

    bool OnEvent(Event event) override {
        if (!m_changes) return false;
        if (event == Event::ArrowUp) {
            move(-1);
            return true;
        }
        if (event == Event::ArrowDown) {
            move(+1);
            return true;
        }
        if (event == Event::Return) {
            if (m_on_jump) m_on_jump();
            return true;
        }
        if (event.is_character() && (event.character() == "e" || event.character() == "E")) {
            m_changes->purpose_expanded = !m_changes->purpose_expanded;
            return true;
        }
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            if (m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Pressed) {
                for (std::size_t k = 0; k < m_item_boxes.size(); ++k) {
                    if (m_item_boxes[k].box.Contain(m.x, m.y)) {
                        m_changes->selected = m_item_boxes[k].index;
                        if (m_on_jump) m_on_jump();
                        return true;
                    }
                }
                if (m_box.Contain(m.x, m.y)) return true;  // 面板内空白点击消费
                return false;
            }
            return false;
        }
        return false;
    }

    Element OnRender() override {
        if (!m_changes) return ftxui::emptyElement();
        m_item_boxes.clear();
        Element el = build_change_viewer(*m_changes, &m_item_boxes);
        return el | ftxui::reflect(m_box);
    }

   private:
    void move(int delta) {
        const int total = static_cast<int>(m_changes->changes.size());
        if (total <= 0) return;
        m_changes->selected = (m_changes->selected + delta) % total;
        if (m_changes->selected < 0) m_changes->selected += total;
    }

    ChangeViewState* m_changes;
    std::function<void()> m_on_jump;
    ftxui::Box m_box;
    std::deque<ChangeHit> m_item_boxes;  ///< 修改点行命中区（reflect 实时回写）
};

}  // namespace

ftxui::Component make_change_viewer(ChangeViewState* changes, std::function<void()> on_jump) {
    return ftxui::Make<ChangeViewer>(changes, std::move(on_jump));
}

}  // namespace ftxtui
