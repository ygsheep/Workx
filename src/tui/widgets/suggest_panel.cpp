/**
 * @file suggest_panel.cpp
 * @brief 输入栏提示面板渲染 + 纯逻辑（parse_suggest_query / filter_commands）
 */

#include "widgets/suggest_panel.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 小写化（过滤用）
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// @brief 面板边框色（弱提示灰）
const Color kBorderColor = theme::T::TextFaint;

}  // namespace

SuggestMode parse_suggest_query(const std::string& line, std::string& query) {
    query.clear();
    if (line.empty()) return SuggestMode::None;
    // 命令模式：行内最后一个 "/"（不要求行首）——"/" 与其后的命令补全路径。
    // 允许 "aaa/xxx"、独立 "/"、/model 等任意位置触发命令面板，避免
    // "只有行首第一次输入才弹"的假象。
    // 例外 1：若该 "/" 属于 "@路径"（其前存在 "@"），则是文件引用里的路径分隔符，
    //          不应当成命令触发符——这类输入继续走文件模式。
    // 例外 2：若该 "/" 之后存在 "@"（如 "/view @path"），说明用户在用 @ 触发
    //          文件搜索作为命令参数，同样优先走文件模式。
    const auto slash = line.rfind('/');
    const auto at = line.rfind('@');
    const bool slash_inside_at_path =
        (at != std::string::npos) && (slash != std::string::npos) && at < slash;
    const bool at_in_cmd_arg =
        (at != std::string::npos) && (slash != std::string::npos) && at > slash;
    if (slash != std::string::npos && !slash_inside_at_path && !at_in_cmd_arg) {
        const std::string cmd_after = line.substr(slash + 1);
        if (cmd_after.find(' ') == std::string::npos) {
            query = cmd_after;
            return SuggestMode::Command;
        }
    }
    // 文件模式：行内最后一个 "@"，其后无空格（对齐 src/tui bottom_bar_manager）
    if (at != std::string::npos) {
        const std::string after = line.substr(at + 1);
        if (after.find(' ') == std::string::npos) {
            query = after;
            return SuggestMode::File;
        }
    }
    return SuggestMode::None;
}

std::vector<size_t> filter_commands(const std::vector<std::string>& commands,
                                    const std::string& query) {
    std::vector<size_t> hits;
    const std::string q = lower(query);
    for (size_t i = 0; i < commands.size(); ++i) {
        if (q.empty() || lower(commands[i]).find(q) != std::string::npos) hits.push_back(i);
    }
    return hits;
}

std::string apply_command_suggest(const std::string& line, const std::string& full) {
    // 定位触发命令面板的最后一个 "/"（与 parse_suggest_query 一致：
    // 非 @路径 内的最后一个 "/"），从该处起替换为完整命令。
    const auto slash = line.rfind('/');
    const auto at = line.rfind('@');
    const bool slash_inside_at_path =
        (at != std::string::npos) && (slash != std::string::npos) && at < slash;
    if (slash != std::string::npos && !slash_inside_at_path)
        return line.substr(0, slash) + full + " ";
    return line + full + " ";
}

Element render_suggest_panel(SuggestMode mode, const std::vector<SuggestEntry>& entries,
                             int selected, bool file_ready, std::deque<ftxui::Box>* hit_boxes) {
    using namespace ftxui;

    if (mode == SuggestMode::None) return emptyElement();
    if (hit_boxes) hit_boxes->clear();

    // 空态文案（区分命令/文件；文件另有「索引构建中」）
    Element body;
    if (entries.empty()) {
        if (mode == SuggestMode::File && !file_ready) {
            body = text(std::string(str::kSuggestIndexing)) | color(theme::T::TextDim);
        } else {
            body = text(mode == SuggestMode::Command ? std::string(str::kSuggestNoCommand)
                                                     : std::string(str::kSuggestNoFile)) |
                   color(theme::T::TextDim);
        }
    } else {
        // 滚动窗口：选中项始终可见（Tab 向下 / Shift+Tab 向上循环到超长列表时不脱视）
        constexpr int kMaxVisible = 8;
        const int total = static_cast<int>(entries.size());
        const int vis = std::min(total, kMaxVisible);
        int start = 0;
        if (selected >= 0) {
            start = std::max(0, selected - (vis - 1) / 2);
            if (start + vis > total) start = std::max(0, total - vis);
        }
        const int shown_end = std::min(start + vis, total);

        Elements rows;
        for (int i = start; i < shown_end; ++i) {
            const auto& e = entries[static_cast<size_t>(i)];
            const bool sel = (i == selected);
            auto row = hbox({
                text(sel ? "  ❯ " : "    "),
                text(e.title) | color(theme::T::Text),
                e.subtitle.empty() ? flex(text("")) : flex(text("")),
                e.subtitle.empty() ? emptyElement()
                                   : text("  " + e.subtitle) | color(theme::T::TextFaint),
                text(" "),
            });
            if (sel) row = row | bgcolor(theme::T::Selection);
            if (hit_boxes) {
                // 记录候选行屏幕 box（deque 保证 reflect 的 Box& 地址稳定）
                hit_boxes->push_back(ftxui::Box{});
                row = row | ftxui::reflect(hit_boxes->back());
            }
            rows.push_back(row);
        }
        const int hidden = total - shown_end;
        if (hidden > 0)
            rows.push_back(text(std::string(str::kPaletteMorePrefix) + std::to_string(hidden) +
                                std::string(str::kPaletteMoreSuffix)) |
                           color(theme::T::TextFaint));
        body = vbox(std::move(rows));
    }

    // 高度上限 8 行；边框与输入区同宽（App 侧以 flex 填充）
    return vbox({
               text(" "),
               hbox({text("  "), body | flex, text("  ")}),
               text(" "),
           }) |
           size(HEIGHT, LESS_THAN, 10) | bgcolor(theme::T::Panel) | border | color(kBorderColor);
}

}  // namespace ftxtui