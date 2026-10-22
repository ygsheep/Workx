/**
 * @file search_palette.cpp
 * @brief 全局聚合搜索面板实现（替代旧 command_palette）
 */

#include "widgets/search_palette.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <optional>
#include <string>
#include <utility>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

namespace {

// 统一主题色（对应 theme/theme.h）
const Color kPanelBg = theme::T::Panel;    // 面板背景
const Color kAccent = theme::T::Accent;    // 「⌕」/光标
const Color kSelBg = theme::T::Selection;  // 选中项背景
constexpr int kMaxVisible = 10;            // 列表最多显示项数

/// @brief 类别标签（行右侧暗色显示）
std::string_view category_label(SearchCategory c) {
    switch (c) {
        case SearchCategory::Feature:
            return str::kCatFeature;
        case SearchCategory::File:
            return str::kCatFile;
        case SearchCategory::Session:
            return str::kCatSession;
        case SearchCategory::Setting:
            return str::kCatSetting;
        case SearchCategory::Model:
            return str::kCatModel;
        case SearchCategory::Provider:
            return str::kCatProvider;
    }
    return "";
}

/// @brief 小写化（过滤用）
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

std::vector<int> filter_search_entries(const std::vector<SearchEntry>& entries,
                                       const std::string& query) {
    // 前缀限定类别：@ 只搜文件，/ 只搜命令/功能（含 skill）；无前缀搜全部
    std::optional<SearchCategory> only;
    std::string term = query;
    if (!query.empty()) {
        if (query[0] == '@') {
            only = SearchCategory::File;
            term = query.substr(1);
        } else if (query[0] == '/') {
            only = SearchCategory::Feature;
            term = query.substr(1);
        }
    }
    const std::string q = lower(term);
    std::vector<int> prefix_hits;
    std::vector<int> sub_hits;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        if (only && e.category != *only) continue;  // 前缀限定类别
        const std::string hay = lower(e.title + " " + e.subtitle + " " + e.keywords);
        if (q.empty() || hay.find(q) != std::string::npos) {
            // 标题前缀命中优先（排序规则 1）；其余保持数据源顺序（规则 2）
            if (!q.empty() && lower(e.title).find(q) == 0)
                prefix_hits.push_back(static_cast<int>(i));
            else
                sub_hits.push_back(static_cast<int>(i));
        }
    }
    prefix_hits.insert(prefix_hits.end(), sub_hits.begin(), sub_hits.end());
    return prefix_hits;
}

/// @brief 类别固定展示顺序（聚合面板分组用；单类别的 /model /resume /provider 不受影响）
constexpr SearchCategory kCategoryOrder[] = {
    SearchCategory::Feature, SearchCategory::File,  SearchCategory::Session,
    SearchCategory::Setting, SearchCategory::Model, SearchCategory::Provider,
};

class SearchPalette : public ftxui::ComponentBase {
   public:
    SearchPalette(std::vector<SearchEntry>& entries, std::function<void(int)> on_select, bool& open,
                  std::function<void()> on_close, std::string title, bool restrict_default)
        : m_all(entries),
          m_on_select(std::move(on_select)),
          m_open(open),
          m_on_close(std::move(on_close)),
          m_title(std::move(title)),
          m_restrict_default(restrict_default) {
        refilter();
    }

    bool OnEvent(Event event) override {
        // Esc：先清空搜索，再关闭
        if (event == Event::Escape) {
            if (!m_search.empty()) {
                m_search.clear();
                refilter();
                return true;
            }
            close();
            return true;
        }
        // Enter：先关面板（on_close 恢复焦点），再执行选中项（动作可再开新面板）
        if (event == Event::Return) {
            run_selected();
            return true;
        }
        // 移动选择：↑↓ / Ctrl+N / Ctrl+P / Tab（Tab = 向下循环，等同 ArrowDown）
        if (event == Event::ArrowUp || event == Event::CtrlP) {
            move(-1);
            return true;
        }
        if (event == Event::ArrowDown || event == Event::CtrlN || event == Event::Tab) {
            move(+1);
            return true;
        }
        // 行编辑
        if (event == Event::CtrlU) {
            m_search.clear();
            refilter();
            return true;
        }
        if (event == Event::CtrlW) {
            delete_word();
            return true;
        }
        if (event == Event::Backspace) {
            if (!m_search.empty()) {
                m_search.pop_back();
                refilter();
            }
            return true;
        }
        // 鼠标：点击列表项选中并运行；滚轮在面板内滚动列表
        if (event.is_mouse()) {
            const auto& m = event.mouse();
            if (m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Pressed) {
                for (size_t k = 0; k < m_item_boxes.size(); ++k) {
                    if (m_item_boxes[k].Contain(m.x, m.y)) {
                        m_selected = m_item_rows[k];
                        run_selected();
                        return true;
                    }
                }
                // 面板内空白点击：消费，避免穿透到下层转录
                if (m_box.Contain(m.x, m.y)) return true;
                return false;
            }
            if (m.button == ftxui::Mouse::WheelUp || m.button == ftxui::Mouse::WheelDown) {
                // 滚轮在面板内滚动列表；在面板外交回上层滚动转录
                if (m_box.Contain(m.x, m.y)) {
                    m_scroll += (m.button == ftxui::Mouse::WheelDown) ? 1 : -1;
                    return true;
                }
                return false;
            }
            return false;
        }
        // 文本输入（UTF-8）
        if (event.is_character()) {
            m_search += event.character();
            refilter();
            return true;
        }
        return false;
    }

    Element OnRender() override {
        using namespace ftxui;

        // 打开边沿：清空搜索框；每次渲染重新过滤（entries 由调用方装配/更新，
        // 如会话列表后台加载完成）。选择位置在过滤后保留（clamp）。
        if (m_open && !m_last_open) {
            m_search.clear();
            m_scroll = 0;
        }
        m_last_open = m_open;
        refilter();
        m_item_boxes.clear();
        m_item_rows.clear();

        // 标题行（面板标题，如「恢复会话」）
        Elements head;
        if (!m_title.empty()) {
            head.push_back(hbox({
                text("  "),
                text(m_title) | color(theme::T::Accent) | bold,
                flex(text("")),
            }));
        }

        // 顶部搜索行（面板聚焦时定位控制台光标到此，使 IME 候选框跟随输入框）
        Element input_field = hbox({
            text("⌕ ") | color(kAccent),
            m_search.empty() ? text(std::string(str::kPaletteSearchHint)) | color(theme::T::Text)
                             : hbox({text(m_search), text("▎") | color(kAccent)}),
            flex(text("")),
        });
        if (Focused()) input_field = ftxui::focusCursorBar(std::move(input_field));

        // 列表（分组 + 滚动窗口，保证选中项可见）
        Elements list;
        const size_t n = m_order.size();
        if (n == 0) {
            list.push_back(text(std::string(str::kPaletteNoMatch)) | color(theme::T::Text));
        } else {
            // 统计不同类别数：单一类别（如 /model /resume /provider）不显示标头，
            // 多类别聚合面板才插入分隔线/标题，突出「会话记录 / 功能 / 文件」区别。
            bool cat_seen[6] = {};
            for (size_t i = 0; i < n; ++i)
                cat_seen[static_cast<int>(m_all[static_cast<size_t>(m_order[i])].category)] = true;
            const int distinct =
                (cat_seen[0] + cat_seen[1] + cat_seen[2] + cat_seen[3] + cat_seen[4] + cat_seen[5]);
            const bool grouped = distinct > 1 || m_title.empty();

            // 逐条逻辑行号（类别切换处插入分隔行）
            int total = 0;
            int sel_row = -1;
            SearchCategory prev = static_cast<SearchCategory>(0);
            bool has_prev = false;
            for (size_t i = 0; i < n; ++i) {
                const SearchCategory cat = m_all[static_cast<size_t>(m_order[i])].category;
                if (grouped && (!has_prev || cat != prev)) ++total;  // 分隔行（含首类别）
                has_prev = true;
                prev = cat;
                if (static_cast<int>(i) == m_selected) sel_row = total;
                ++total;
            }

            const int visible = std::min<int>(total, kMaxVisible);
            // 滚轮偏移叠加在「选中项居中」之上，clamp 到合法窗口范围
            m_scroll = std::clamp(m_scroll, 0, std::max(0, total - visible));
            int start = 0;
            if (sel_row >= 0) start = std::max(0, sel_row - (visible - 1) / 2);
            start = std::clamp(start + m_scroll, 0, std::max(0, total - visible));
            const int shown_end = std::min(start + visible, total);

            int cur = 0;
            has_prev = false;
            for (size_t i = 0; i < n; ++i) {
                const SearchCategory cat = m_all[static_cast<size_t>(m_order[i])].category;
                if (grouped && (!has_prev || cat != prev)) {
                    if (cur >= start && cur < shown_end) {
                        // 分隔线：类别标题 + 延伸线
                        list.push_back(hbox({
                            text(" "),
                            text(std::string(category_label(cat))) | color(theme::T::TextDim) |
                                bold,
                            flex(text("─") | color(theme::T::TextFaint)),
                        }));
                    }
                    ++cur;
                }
                has_prev = true;
                prev = cat;
                if (cur >= start && cur < shown_end) {
                    const SearchEntry& se = m_all[static_cast<size_t>(m_order[i])];
                    const bool sel = (static_cast<int>(i) == m_selected);
                    // 使用中条目行首 ●（accent 色），选中行 ❯，其余空白
                    std::string marker = se.active ? "  ● " : (sel ? "  ❯ " : "    ");
                    auto marker_elem =
                        text(marker) | color(se.active ? theme::T::Accent : ftxui::Color::Default);
                    auto row = hbox({
                        marker_elem,
                        text(se.title.empty() ? se.subtitle : se.title),
                        flex(text("")),
                        se.subtitle.empty()
                            ? emptyElement()
                            : (text(" " + se.subtitle) | color(theme::T::TextFaint)),
                        text("  ") | color(theme::T::TextFaint),
                        text(std::string(category_label(se.category))) | color(theme::T::TextFaint),
                        text(" "),
                    });
                    if (sel) row = row | bgcolor(kSelBg) | color(theme::T::Text) | bold;
                    // 记录可见项 box（deque 保证 reflect 引用的地址稳定），供鼠标点击命中
                    m_item_boxes.emplace_back();
                    m_item_rows.push_back(static_cast<int>(i));
                    list.push_back(row | ftxui::reflect(m_item_boxes.back()));
                }
                ++cur;
            }
            const int hidden = total - shown_end;
            if (hidden > 0)
                list.push_back(text(std::string(str::kPaletteMorePrefix) + std::to_string(hidden) +
                                    std::string(str::kPaletteMoreSuffix)) |
                               color(theme::T::Text));
        }

        Elements content_elems;
        for (auto& h : head) content_elems.push_back(std::move(h));
        content_elems.push_back(input_field);
        content_elems.push_back(separatorEmpty());
        content_elems.push_back(vbox(std::move(list)));
        content_elems.push_back(separatorEmpty());
        content_elems.push_back(text(std::string(str::kPaletteHint)) | color(theme::T::Text));
        auto content = vbox(std::move(content_elems));

        // 宽度固定为终端宽度的 60%（小屏保留下限），高度按可见条目动态收束。
        // 使用 ftxui::Terminal::Size()：app 每帧已用 CONOUT$ 校准 fallback 尺寸，
        // 故此处取到的即为真实终端宽度。
        const int term_w = ftxui::Terminal::Size().dimx;
        const int panel_w = std::max(40, term_w * 60 / 100);
        auto panel = vbox({
                         text(" "),
                         hbox({text("  "), content | flex, text("  ")}),
                         text(" "),
                     }) |
                     size(WIDTH, EQUAL, panel_w) | size(HEIGHT, LESS_THAN, kMaxVisible + 6) |
                     bgcolor(kPanelBg) | border;
        // 悬浮面板必须清除下层会话内容：否则上层格只有背景色而无前景时，
        // FTXUI 合成会透出下层文本字符，造成「背景被穿透、文本重叠」。
        // 所有悬浮面板（/model /resume /provider /Ctrl+P）统一处理。
        // reflect 捕获面板实际渲染 box（含居中定位），供鼠标命中测试。
        return ftxui::clear_under(panel) | ftxui::reflect(m_box);
    }

   private:
    void refilter() {
        // 先做前缀过滤（@→文件 /→功能），再按类别固定顺序分组聚合，
        // 保证同一类别连续显示（渲染层据此插入分隔线）。
        const auto hits = filter_search_entries(m_all, m_search);
        m_order.clear();
        m_order.reserve(hits.size());
        // 聚合面板默认去噪：搜索框为空时仅显示「会话记录 / 设置」，输入后恢复全类；
        // 单类别面板（/model /resume /provider）不受影响（restrict_default=false）。
        const bool restrict_empty = m_restrict_default && m_search.empty();
        for (SearchCategory cat : kCategoryOrder)
            for (int idx : hits)
                if (m_all[static_cast<size_t>(idx)].category == cat) {
                    if (restrict_empty && cat != SearchCategory::Session &&
                        cat != SearchCategory::Setting)
                        continue;
                    m_order.push_back(idx);
                }
        if (m_order.empty()) {
            m_selected = -1;
        } else {
            if (m_selected < 0) m_selected = 0;
            m_selected = std::min(m_selected, static_cast<int>(m_order.size()) - 1);
        }
    }

    void move(int delta) {
        if (m_order.empty()) return;
        const int n = static_cast<int>(m_order.size());
        m_selected = (m_selected + delta) % n;
        if (m_selected < 0) m_selected += n;
    }

    void delete_word() {
        size_t i = m_search.size();
        while (i > 0 && m_search[i - 1] == ' ') --i;
        while (i > 0 && m_search[i - 1] != ' ') --i;
        if (i == m_search.size()) return;  // 已到行首
        m_search.erase(i);
        refilter();
    }

    void close() {
        m_open = false;
        if (m_on_close) m_on_close();
    }

    /// @brief 关闭面板并执行当前选中项（Enter / 鼠标点击共用）
    void run_selected() {
        const int idx = m_selected;
        close();
        if (idx >= 0 && static_cast<size_t>(idx) < m_order.size())
            if (m_on_select) m_on_select(m_order[static_cast<size_t>(idx)]);
    }

    // 滚动窗口在 OnRender 每帧根据选中项计算（m_start 为逻辑行偏移），不单独维护。

    std::vector<SearchEntry>& m_all;
    std::vector<int> m_order;  // 分组后位置 → m_all 原始下标
    std::string m_search;
    std::string m_title;              // 面板标题（搜索框上方）
    bool m_restrict_default = false;  // 聚合面板默认去噪：空查询仅会话/设置
    int m_selected = -1;
    int m_scroll = 0;          // 滚轮手动滚动偏移（叠加在选中项居中之上）
    bool m_last_open = false;  // 打开边沿检测（清空搜索框）
    ftxui::Box m_box;          // 面板实际渲染 box（reflect 捕获，鼠标命中）
    std::deque<ftxui::Box> m_item_boxes;  // 可见项 box（deque 保证引用地址稳定）
    std::deque<int> m_item_rows;          // 可见项 → m_order 下标
    std::function<void(int)> m_on_select;
    bool& m_open;
    std::function<void()> m_on_close;
};

ftxui::Component make_search_palette(std::vector<SearchEntry>& entries,
                                     std::function<void(int)> on_select, bool& open,
                                     std::function<void()> on_close, std::string title,
                                     bool restrict_default) {
    return ftxui::Make<SearchPalette>(entries, std::move(on_select), open, std::move(on_close),
                                      std::move(title), restrict_default);
}

}  // namespace ftxtui