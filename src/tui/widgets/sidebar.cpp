#include "widgets/sidebar.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/direction.hpp>

#include "core/todo/todo_item.h"  // #24：待办状态图标渲染
#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
/// 信息行文字：米白色（统一主题）
const Color kInfoText = theme::T::Text;

/// @brief 截断到 max 字符（超长加省略号；回退到多字节边界避免切断 UTF-8）
std::string truncate_utf8(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    s.resize(max);
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
    if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) s.pop_back();
    s += "…";
    return s;
}

/// @brief 键值行；默认键灰暗、值默认色；传入 text 时整行（键+值）用该颜色
Element kv(const std::string& key, const std::string& value, Color text = Color::Default) {
    auto key_el = ftxui::text(key);
    auto val_el = ftxui::text(value);
    if (text == Color::Default) {
        key_el |= ftxui::color(theme::T::Text);
        val_el |= ftxui::color(theme::T::Text);
    } else {
        key_el |= ftxui::color(text);
        val_el |= ftxui::color(text);
    }
    return ftxui::hbox({
        key_el,
        ftxui::separatorEmpty(),
        ftxui::flex(val_el),
    });
}

/// @brief 区块标题：米白
Element section_title(const std::string_view& title) {
    return ftxui::text(std::string(title)) | ftxui::color(kInfoText);
}

/// @brief token 格式化：>=1000 显示 x.xk，否则原样
std::string fmt_k(int32_t n) {
    if (n >= 1000) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0fk", n / 1000.0);
        return buf;
    }
    return std::to_string(n);
}

/// @brief 上下文进度条（对齐 src/tui statusbar：10 段，四阶段色）
///        0-50% 绿 / 50-80% 黄 / 80-90% 橙 / 90-100% 红
Element context_bar(int used, int limit) {
    if (limit <= 0) return ftxui::text(std::string(str::kGaugeNA));
    double ratio = std::clamp(static_cast<double>(used) / limit, 0.0, 1.0);
    int filled = static_cast<int>(ratio * 10.0);
    Color bar_color;
    if (ratio < 0.5) {
        bar_color = ftxui::Color::Green;
    } else if (ratio < 0.8) {
        bar_color = ftxui::Color::Yellow;
    } else if (ratio < 0.9) {
        bar_color = ftxui::Color::RGB(0xFF, 0xA5, 0x00);  // 橙
    } else {
        bar_color = ftxui::Color::Red;
    }
    Element bar = ftxui::emptyElement();
    for (int i = 0; i < 10; ++i) {
        if (i < filled) {
            bar = ftxui::hbox({bar, ftxui::text("█") | ftxui::color(bar_color)});
        } else {
            // 未使用部分：灰色背景实心块（非颗粒，需无颗粒前景字符）
            bar = ftxui::hbox({bar, ftxui::text(" ") | ftxui::bgcolor(theme::T::TextFaint)});
        }
    }
    return bar;
}

/// @brief 上下文信息区（statusbar 风格）：进度条 + 百分比 + 用量 + Token/缓存
/// @note 仅保留 token + 上下文使用大小 + 缓存（成本/Agent 已移除）
void append_context_info(Elements& rows, const SidebarModel& s) {
    bool has_limit = s.context_limit > 0;
    int limit = has_limit ? s.context_limit : 0;
    double pct_d =
        has_limit ? std::min(static_cast<double>(s.context_used) * 100.0 / limit, 100.0) : 0.0;
    int pct = static_cast<int>(pct_d);

    // 上下文行：`上下文 ▓▓▓▓▓░░░░░ 6% 12k/200k`
    std::string ctx_abs =
        has_limit ? (fmt_k(s.context_used) + "/" + fmt_k(limit)) : ("~" + fmt_k(s.context_used));
    char pct_buf[16];
    if (has_limit) {
        if (pct_d < 1.0 && s.context_used > 0)
            snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct_d);
        else
            snprintf(pct_buf, sizeof(pct_buf), "%d%%", pct);
    }
    rows.push_back(ftxui::hbox({
        ftxui::text(std::string(str::kSidebarContext)) | ftxui::color(kInfoText),
        ftxui::text(" "),
        context_bar(s.context_used, s.context_limit),
        ftxui::text(" "),
        has_limit ? ftxui::text(pct_buf) | ftxui::color(theme::T::TextDim)
                  : ftxui::text(std::string(str::kDash)) | ftxui::color(theme::T::TextDim),
        ftxui::text(" "),
        ftxui::text(ctx_abs) | ftxui::color(theme::T::TextFaint),
    }));

    // Token + 缓存行：`Token 12k · cache 8k`
    std::string cache_str;
    if (s.cache_read_tokens > 0) cache_str = " · cache " + fmt_k(s.cache_read_tokens);
    rows.push_back(ftxui::hbox({
        ftxui::text(std::string(str::kSidebarToken)) | ftxui::color(kInfoText),
        ftxui::text(" "),
        ftxui::text(fmt_k(s.total_tokens)) | ftxui::color(theme::T::TextDim),
        ftxui::text(cache_str) | ftxui::color(theme::T::TextFaint),
    }));

    // #65：DS 缓存命中率行：`DS 缓存 命中 80% (8k/10k)`
    const int64_t cache_total = static_cast<int64_t>(s.cache_hit_tokens) + s.cache_miss_tokens;
    if (cache_total > 0) {
        const int hit_pct = static_cast<int>(
            (static_cast<int64_t>(s.cache_hit_tokens) * 100 + cache_total / 2) / cache_total);
        char hit_buf[16];
        snprintf(hit_buf, sizeof(hit_buf), "%d%%", hit_pct);
        const std::string hit_str = std::string(str::kSidebarHitRate) + " " + hit_buf;
        const std::string detail_str =
            " (" + fmt_k(s.cache_hit_tokens) + "/" + fmt_k(static_cast<int32_t>(cache_total)) + ")";
        rows.push_back(ftxui::hbox({
            ftxui::text(std::string(str::kSidebarCache)) | ftxui::color(kInfoText),
            ftxui::text(" "),
            ftxui::text(hit_str) | ftxui::color(theme::T::TextDim),
            ftxui::text(detail_str) | ftxui::color(theme::T::TextFaint),
        }));
    }

    // #65：Prompt/生成分项行：`Prompt 10k · 生成 2k`
    if (s.prompt_tokens > 0 || s.generated_tokens > 0) {
        std::string gen_str;
        if (s.generated_tokens > 0)
            gen_str = " · " + std::string(str::kSidebarGenerated) + " " + fmt_k(s.generated_tokens);
        rows.push_back(ftxui::hbox({
            ftxui::text(std::string(str::kSidebarPrompt)) | ftxui::color(kInfoText),
            ftxui::text(" "),
            ftxui::text(fmt_k(s.prompt_tokens)) | ftxui::color(theme::T::TextDim),
            ftxui::text(gen_str) | ftxui::color(theme::T::TextFaint),
        }));
    }
}

/// @brief 可折叠区块（MCP）：chevron 标题行可点击，展开显示条目
/// @details 每台 server 前带状态点（绿=已连接 / 红=失败 / 灰=连接中），
///          失败时在下方追加红色错误信息行。
void append_mcp_section(Elements& rows, const std::vector<McpServerEntry>& servers, bool expanded,
                        SectionHit* hit) {
    Element header = ftxui::hbox({
        ftxui::text(
            std::string(expanded ? theme::icon_chevron_down() : theme::icon_chevron_right())) |
            ftxui::color(theme::T::TextFaint),
        ftxui::text(" "),
        ftxui::text(std::string(str::kSidebarMCP)) | ftxui::color(kInfoText),
    });
    if (hit) header = header | ftxui::reflect(hit->box);
    rows.push_back(header);
    if (!expanded) return;
    if (servers.empty()) {
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(str::kDash)) | ftxui::color(theme::T::TextFaint),
        }));
        return;
    }
    for (const auto& s : servers) {
        // 状态点：1=已连接(绿) 2=失败(红) 0=连接中(灰)
        Color dot_color = theme::T::TextFaint;
        if (s.state == 1)
            dot_color = theme::T::DiffAdd;
        else if (s.state == 2)
            dot_color = theme::T::DiffDel;
        std::string line = s.name;
        if (!s.protocol.empty()) line += " · " + s.protocol;
        if (s.tool_count > 0) line += " · " + std::to_string(s.tool_count) + " 工具";
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(theme::icon_dot())) | ftxui::color(dot_color),
            ftxui::text(" "),
            ftxui::flex(ftxui::text(line) | ftxui::color(theme::T::TextDim)),
        }));
        // 失败：红色错误信息（截断避免溢出窄侧栏）
        if (s.state == 2 && !s.error.empty()) {
            rows.push_back(ftxui::hbox({
                ftxui::text("    "),
                ftxui::flex(ftxui::text(truncate_utf8(s.error, 48)) |
                            ftxui::color(theme::T::DiffDel)),
            }));
        }
    }
}

/// @brief 待办状态图标（#24：✓ 完成 / ▶ 进行中 / ○ 未开始）
std::pair<std::string_view, Color> todo_status_icon(core::todo::TodoStatus s) {
    switch (s) {
        case core::todo::TodoStatus::Completed:
            return {"✓", theme::T::DiffAdd};  // 绿
        case core::todo::TodoStatus::InProgress:
            return {"▶", theme::T::Accent};  // 蓝
        default:
            return {"○", theme::T::TextFaint};  // 灰
    }
}

/// @brief TODO 区块（#24）：状态图标 + 内容，展开显示条目
void append_todo_section(Elements& rows, const std::vector<core::todo::TodoItem>& todos,
                         bool expanded, SectionHit* hit) {
    Element header = ftxui::hbox({
        ftxui::text(
            std::string(expanded ? theme::icon_chevron_down() : theme::icon_chevron_right())) |
            ftxui::color(theme::T::TextFaint),
        ftxui::text(" "),
        ftxui::text(std::string(str::kSidebarTODO)) | ftxui::color(kInfoText),
    });
    if (hit) header = header | ftxui::reflect(hit->box);
    rows.push_back(header);
    if (!expanded) return;
    if (todos.empty()) {
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(str::kDash)) | ftxui::color(theme::T::TextFaint),
        }));
        return;
    }
    for (const auto& t : todos) {
        auto [icon, color] = todo_status_icon(t.status);
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(icon)) | ftxui::color(color),
            ftxui::text(" "),
            ftxui::flex(ftxui::text(t.content) | ftxui::color(theme::T::TextDim)),
        }));
    }
}
}  // namespace

void append_sidebar_info(Elements& rows, const SidebarModel& s,
                         std::deque<SectionHit>* section_hits) {
    if (section_hits) section_hits->clear();

    Elements inner;

    // 标题
    std::string title = s.title.empty() ? std::string(str::kSidebarNewSession) : s.title;
    inner.push_back(ftxui::text(title) | ftxui::bold);

    // 基础信息：项目 / 分支 / 模型（Agent 已按用户要求移除）
    inner.push_back(kv(std::string(str::kSidebarProject),
                       s.project.empty() ? std::string(str::kDash) : s.project, kInfoText));
    if (!s.branch.empty()) inner.push_back(kv(std::string(str::kSidebarBranch), s.branch));
    inner.push_back(kv(std::string(str::kSidebarModel),
                       s.model.empty() ? std::string(str::kDash) : s.model, kInfoText));

    // 上下文信息：token + 上下文使用大小 + 缓存（statusbar 风格）
    append_context_info(inner, s);

    // MCP 列表（可折叠）
    SectionHit* mcp_hit = nullptr;
    if (section_hits) {
        section_hits->push_back(SectionHit{});
        section_hits->back().kind = SectionHit::Kind::kMCP;
        mcp_hit = &section_hits->back();
    }
    append_mcp_section(inner, s.mcp_servers, s.mcp_expanded, mcp_hit);

    // TODO 列表（可折叠，#24：状态图标 + 内容）
    SectionHit* todo_hit = nullptr;
    if (section_hits) {
        section_hits->push_back(SectionHit{});
        section_hits->back().kind = SectionHit::Kind::kTODO;
        todo_hit = &section_hits->back();
    }
    append_todo_section(inner, s.todos, s.todo_expanded, todo_hit);

    // 左侧内边距（2 格），与任务调度 tab 其他行对齐
    rows.push_back(ftxui::hbox({
        ftxui::text("  "),
        ftxui::flex(ftxui::vbox(std::move(inner))),
    }));
}

}  // namespace ftxtui
