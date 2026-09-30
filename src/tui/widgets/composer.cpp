/**
 * @file composer.cpp
 * @brief 底部输入组件（自定义渲染：块状光标 + 灰色提示 + 快捷键处理）
 * @details 自绘单行输入，避免 ftxui::Input 的 Windows 光标回退问题
 *          （空输入时光标落在终端右下角）与默认 Bar 光标。
 *          Enter 提交；Tab 切换工作模式（标准/极简/计划）；Shift+Tab 切换权限；
 *          Ctrl+O 思考视图。
 *          Ctrl+P 不在输入栏消费（全局搜索面板由 App 顶层捕获）。
 *          输入栏提示面板（"/" 命令 / "@" 文件）不占焦点：导航键在面板
 *          激活时经 suggest_* 回调转发给 App 状态机，否则走原逻辑。
 */

#include "widgets/composer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Component;
using ftxui::Element;
using ftxui::Event;

namespace {
/// @brief 输入区背景色（与侧边栏一致）
const Color kPanelBg = theme::T::Panel;

/// @brief 光标位置 pos 处字符的字节长度（UTF-8 安全；pos 越界返回 0）
size_t char_len_at(const std::string& s, size_t pos) {
    if (pos >= s.size()) return 0;
    size_t i = pos + 1;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i - pos;
}

/// @brief 返回 pos 前一个字符的字节起点（UTF-8 安全）
size_t prev_char(const std::string& s, size_t pos) {
    if (pos == 0 || pos > s.size()) return 0;
    size_t i = pos;
    while (i > 0) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return i;
    }
    return 0;
}

/// @brief 文本总行数（空文本视为 1 行）
size_t line_count(const std::string& s) {
    if (s.empty()) return 1;
    size_t n = 1;
    for (char c : s)
        if (c == '\n') ++n;
    return n;
}

/// @brief 第 line_idx 行的字节区间 [start, end)（end 不含换行符）
std::pair<size_t, size_t> line_range(const std::string& s, size_t line_idx) {
    size_t start = 0, line = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            if (line == line_idx) return {start, i};
            start = i + 1;
            ++line;
        }
    }
    return {s.size(), s.size()};
}

/// @brief 光标所在行号
size_t cursor_line(const std::string& s, size_t cursor) {
    size_t line = 0;
    for (size_t i = 0; i < cursor && i < s.size(); ++i)
        if (s[i] == '\n') ++line;
    return line;
}

/// @brief [start, cursor) 区间内的字符数（UTF-8 安全，用作列位置）
size_t utf8_col(const std::string& s, size_t start, size_t cursor) {
    size_t col = 0;
    for (size_t i = start; i < cursor; ++i)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++col;
    return col;
}

/// @brief 从 start 起第 col 个字符的字节位置（不超过 end）
size_t byte_pos_at_col(const std::string& s, size_t start, size_t col, size_t end) {
    size_t i = start, c = 0;
    while (i < end && c < col) {
        i += char_len_at(s, i);
        ++c;
    }
    return std::min(i, end);
}

/// @brief 块状光标（主题强调色实心单元，与输入框左边框同色 #5c9cf5）
Element cursor_block() { return ftxui::text(" ") | ftxui::bgcolor(theme::T::Accent); }

/// @brief 把控制台/IME 光标锚定到给定元素所在单元（focused 时）。
///        focusCursorBar 会接管本组件的 focused 游标节点，FTXUI 据此把
///        真实控制台光标定位到该节点左上角。须只作用于"光标所在的那个字符
///        单元"，而不是整条输入元素——否则锚定到行首第 0 列，Windows IME
///        候选/拼音会显示在输入最前面而非输入点之后。
Element mark_cursor(Element e) { return ftxui::focusCursorBar(std::move(e)); }

/// @brief 渲染单行带块状光标的文本（focused 才显示光标）
///        空行：光标块；非空：光标块覆盖当前字符
Element render_line_with_cursor(const std::string& line, size_t col, bool focused) {
    col = std::min(col, line.size());
    if (line.empty()) {
        if (!focused) return ftxui::text(" ");
        return mark_cursor(cursor_block());
    }
    if (col >= line.size()) {
        Element e = ftxui::text(line) | ftxui::color(theme::T::Text);
        if (focused) return ftxui::hbox({e, mark_cursor(cursor_block())});
        return e;
    }
    size_t clen = char_len_at(line, col);
    Element at = ftxui::text(line.substr(col, clen));
    if (focused) {
        at = mark_cursor(at | ftxui::bgcolor(theme::T::Accent) | ftxui::color(Color::White));
    } else {
        at |= ftxui::color(theme::T::Text);
    }
    return ftxui::hbox({
        ftxui::text(line.substr(0, col)) | ftxui::color(theme::T::Text),
        at,
        ftxui::text(line.substr(col + clen)) | ftxui::color(theme::T::Text),
    });
}

/// @brief 渲染带块状光标的输入文本（多行；focused 才显示光标）
///        空输入：光标块 + 灰色提示；非空：按 \n 拆行，光标所在行显示块状光标
Element render_with_cursor(const std::string& text, size_t cursor, bool focused) {
    cursor = std::min(cursor, text.size());
    if (text.empty()) {
        Element ph =
            ftxui::text(std::string(str::kComposerPlaceholder)) | ftxui::color(theme::T::Text);
        if (!focused) return ph;
        return ftxui::hbox({mark_cursor(cursor_block()), ph});
    }
    const size_t nlines = line_count(text);
    const size_t cline = cursor_line(text, cursor);
    std::vector<Element> rows;
    rows.reserve(nlines);
    for (size_t li = 0; li < nlines; ++li) {
        const auto [start, end] = line_range(text, li);
        const std::string line = text.substr(start, end - start);
        if (li == cline)
            rows.push_back(render_line_with_cursor(line, cursor - start, focused));
        else
            rows.push_back(ftxui::text(line) | ftxui::color(theme::T::Text));
    }
    return ftxui::vbox(std::move(rows));
}
}  // namespace

Component make_composer(ComposerOptions& opt) {
    size_t internal_cursor = 0;  ///< 内部光标（opt.cursor 为空时使用）
    size_t& cursor = opt.cursor ? *opt.cursor : internal_cursor;

    // 粘贴脉冲检测：终端粘贴的多行容被拆成多个字符事件快速到达，其中的换行
    // 又会被解析成 Event::Return。靠"与上一次字符插入间隔极短且连续多次"
    // 判定当前处于粘贴中，从而把粘贴换行当作空格插入而非提交。
    // 状态必须堆分配并按值捕获：make_composer 返回后局部变量即失效，
    // 若捕获其引用，每次按键写已释放的栈内存会破坏调用栈（表现为
    // RunOnce 复制组件 shared_ptr 时访问违例）。
    struct PasteState {
        std::chrono::steady_clock::time_point last_insert{};
        int paste_burst = 0;
    };
    auto paste = std::make_shared<PasteState>();
    size_t desired_col = 0;  ///< 跨行移动时保持的目标列（字符数）

    auto renderer = ftxui::Renderer([&opt, &cursor](bool focused) {
        Element body = render_with_cursor(*opt.buffer, cursor, focused) | ftxui::bgcolor(kPanelBg);
        // 控制台/IME 光标的位置已由 render_with_cursor 中的 mark_cursor 锚定到
        // 插入点所在字符单元（而非整条输入元素行首），Windows IME 候选框才会
        // 跟随显示在真实插入位置。此处的 focusCursorBar 不再单独包裹。
        return body;
    });

    Component wrapped =
        renderer | ftxui::CatchEvent([&opt, &cursor, paste, &desired_col, renderer](Event e) {
            try {
                std::string& text = *opt.buffer;
                cursor = std::min(cursor, text.size());

                // ---- 提示面板优先：激活时拦截导航键（面板不占焦点，事件经此处转发）----
                const bool suggest = opt.suggest_active && opt.suggest_active();

                if (e == Event::Return) {
                    // 粘贴多行里的换行（\r\n → \n → Return）：不提交，插入真实换行。
                    // 判定条件：刚从连续快速字符插入（粘贴脉冲）中收到 Return。
                    const bool is_paste_newline =
                        paste->paste_burst >= 2 &&
                        paste->last_insert != std::chrono::steady_clock::time_point{} &&
                        (std::chrono::steady_clock::now() - paste->last_insert) <=
                            std::chrono::milliseconds(150);
                    if (is_paste_newline) {
                        text.insert(cursor, "\n");
                        cursor += 1;
                        if (opt.suggest_refresh) opt.suggest_refresh();
                        return true;
                    }
                    if (suggest && opt.suggest_enter && opt.suggest_enter()) return true;
                    std::string t = text;
                    size_t b = t.find_first_not_of(" \t");
                    size_t en = t.find_last_not_of(" \t");
                    if (b == std::string::npos) return true;  // 全空白
                    std::string trimmed = t.substr(b, en - b + 1);
                    if (opt.on_submit) opt.on_submit(trimmed);
                    text.clear();
                    cursor = 0;
                    if (opt.suggest_refresh) opt.suggest_refresh();
                    return true;
                }
                // Shift+Enter（Windows 补丁改写为 kitty 序列 \x1b[13;2u）：插入换行
                if (e == ftxui::Event::Special("\x1b[13;2u")) {
                    text.insert(cursor, "\n");
                    cursor += 1;
                    if (opt.suggest_refresh) opt.suggest_refresh();
                    return true;
                }
                // Ctrl+Enter（Windows 补丁改写为 kitty 序列 \x1b[13;5u）：
                // 面板激活时插入引用（@路径），否则提交并请求立即冲刷
                // （模型忙碌时入队 + 请求工具轮边界注入；空闲时等同 Enter 直接发送）
                if (e == ftxui::Event::Special("\x1b[13;5u")) {
                    if (suggest && opt.suggest_enter_insert && opt.suggest_enter_insert())
                        return true;
                    std::string t = text;
                    size_t b = t.find_first_not_of(" \t");
                    size_t en = t.find_last_not_of(" \t");
                    if (b == std::string::npos) return true;  // 全空白
                    std::string trimmed = t.substr(b, en - b + 1);
                    if (opt.on_submit_ctrl) opt.on_submit_ctrl(trimmed);
                    text.clear();
                    cursor = 0;
                    if (opt.suggest_refresh) opt.suggest_refresh();
                    return true;
                }
                if (e == Event::Tab) {
                    if (suggest && opt.suggest_move) {
                        opt.suggest_move(+1);  // Tab = 向下循环选择
                        return true;
                    }
                    // 无面板时 Tab = 切换工作模式（标准 → 极简 → 计划）
                    if (opt.on_mode_toggle) opt.on_mode_toggle();
                    return true;
                }
                if (e == Event::Escape) {
                    if (suggest && opt.suggest_cancel) {
                        opt.suggest_cancel();
                        return true;
                    }
                    return false;
                }
                if (e == Event::ArrowUp) {
                    if (suggest && opt.suggest_move) {
                        opt.suggest_move(-1);
                        return true;
                    }
                    const size_t cline = cursor_line(text, cursor);
                    if (cline > 0) {
                        const auto [start, end] = line_range(text, cline);
                        const auto [pstart, pend] = line_range(text, cline - 1);
                        desired_col = utf8_col(text, start, cursor);
                        cursor = byte_pos_at_col(text, pstart, desired_col, pend);
                        return true;
                    }
                    // 已在第一行：优先历史回退（App 侧），否则交给 App 全局处理
                    if (opt.on_history_prev && opt.on_history_prev()) return true;
                    return false;
                }
                if (e == Event::ArrowDown) {
                    if (suggest && opt.suggest_move) {
                        opt.suggest_move(+1);
                        return true;
                    }
                    const size_t cline = cursor_line(text, cursor);
                    if (cline + 1 < line_count(text)) {
                        const auto [start, end] = line_range(text, cline);
                        const auto [nstart, nend] = line_range(text, cline + 1);
                        desired_col = utf8_col(text, start, cursor);
                        cursor = byte_pos_at_col(text, nstart, desired_col, nend);
                        return true;
                    }
                    // 已在最后一行：优先历史前进（App 侧），否则交给 App 全局处理
                    if (opt.on_history_next && opt.on_history_next()) return true;
                    return false;
                }

                if (e == Event::Backspace) {
                    if (cursor > 0) {
                        size_t p = prev_char(text, cursor);
                        text.erase(p, cursor - p);
                        cursor = p;
                    }
                    if (opt.suggest_refresh) opt.suggest_refresh();
                    return true;
                }
                if (e == Event::Delete) {
                    if (cursor < text.size()) text.erase(cursor, char_len_at(text, cursor));
                    if (opt.suggest_refresh) opt.suggest_refresh();
                    return true;
                }
                if (e == Event::ArrowLeft) {
                    cursor = prev_char(text, cursor);
                    return true;
                }
                if (e == Event::ArrowRight) {
                    size_t n = char_len_at(text, cursor);
                    cursor = std::min(text.size(), cursor + std::max<size_t>(1, n));
                    return true;
                }
                if (e == Event::Home) {
                    cursor = line_range(text, cursor_line(text, cursor)).first;
                    return true;
                }
                if (e == Event::End) {
                    cursor = line_range(text, cursor_line(text, cursor)).second;
                    return true;
                }
                if (e == Event::TabReverse) {  // Shift+Tab：提示面板激活时向上循环，否则切换权限
                    if (suggest && opt.suggest_move) {
                        opt.suggest_move(-1);
                        return true;
                    }
                    if (opt.on_perm_toggle) opt.on_perm_toggle();
                    return true;
                }
                // Ctrl 组合键：终端/VT 模式下可能以 Character 或 Special 形式到达
                auto is_ctrl = [&e](uint8_t c) {
                    const std::string s(1, static_cast<char>(c));
                    return (e.is_character() && e.character() == s) ||
                           e == ftxui::Event::Special(s);
                };
                if (is_ctrl(0x14)) {  // Ctrl+T：工作模式切换（标准 → 极简 → 计划）
                    if (opt.on_mode_toggle) opt.on_mode_toggle();
                    return true;
                }
                if (is_ctrl(0x0f)) {  // Ctrl+O 思考视图
                    if (opt.on_toggle_thinking) opt.on_toggle_thinking();
                    return true;
                }
                if (is_ctrl(0x07)) {  // Ctrl+G：打开系统默认编辑器编辑当前输入
                    if (opt.on_edit) opt.on_edit();
                    return true;
                }
                if (is_ctrl(0x01)) {  // Ctrl+A 行首
                    cursor = line_range(text, cursor_line(text, cursor)).first;
                    return true;
                }
                if (is_ctrl(0x05)) {  // Ctrl+E 行尾
                    cursor = line_range(text, cursor_line(text, cursor)).second;
                    return true;
                }
                if (e.is_character()) {
                    const std::string& ch = e.character();
                    if (ch.size() == 1 && static_cast<uint8_t>(ch[0]) < 0x20) return false;
                    // 维护粘贴脉冲计数：距上次插入 ≤100ms 视为连续（粘贴），否则重新计数
                    const auto now = std::chrono::steady_clock::now();
                    if (paste->last_insert != std::chrono::steady_clock::time_point{} &&
                        now - paste->last_insert <= std::chrono::milliseconds(100)) {
                        ++paste->paste_burst;
                    } else {
                        paste->paste_burst = 1;
                    }
                    paste->last_insert = now;
                    text.insert(cursor, ch);
                    cursor += ch.size();
                    if (opt.suggest_refresh) opt.suggest_refresh();
                    return true;
                }
                if (e.is_mouse()) {
                    if (e.mouse().button == ftxui::Mouse::Left &&
                        e.mouse().motion == ftxui::Mouse::Pressed) {
                        renderer->TakeFocus();
                        return true;
                    }
                }
                return false;
            } catch (...) {
                // 单次按键处理即便抛出（含非 std::exception 类型），也不应令整个程序退出
                return true;
            }
        });

    return wrapped;
}

}  // namespace ftxtui