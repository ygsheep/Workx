#include "widgets/sidebar_tabs.h"

#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/direction.hpp>

#include "theme/strings.h"
#include "theme/theme.h"
#include "widgets/change_viewer.h"
#include "widgets/file_viewer.h"
#include "widgets/project_tree.h"
#include "widgets/sidebar.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 键值行：键灰暗、值默认色（对齐原侧栏 kv 风格）
Element kv_line(std::string_view key, std::string_view value) {
    return ftxui::hbox({
        ftxui::text("  "),
        ftxui::text(std::string(key)) | ftxui::color(theme::T::TextFaint),
        ftxui::text(" "),
        ftxui::flex(ftxui::text(std::string(value)) | ftxui::color(theme::T::TextDim)),
    });
}

/// @brief 区块标题：米白
Element section_title(const std::string_view& title) {
    return ftxui::text(std::string(title)) | ftxui::color(theme::T::Text);
}

/// @brief 单个 tab 块：` 标签 [ ✕] `；选中项白色边框线 + 米白，未选中无边框线（占位）
/// @param hit 非空时记录整块命中区（点击切换）
/// @param close_hit 非空且可关闭时记录 ✕ 命中区（点击关闭）
Element tab_block(const std::string& label, bool selected, bool closable, TabHit* hit,
                  TabHit* close_hit) {
    Elements parts;
    parts.push_back(ftxui::text(" "));
    parts.push_back(ftxui::text(label) |
                    ftxui::color(selected ? theme::T::Text : theme::T::TextDim));
    if (closable) {
        parts.push_back(ftxui::text(" "));
        Element close_el =
            ftxui::text(std::string(str::kTabClose)) | ftxui::color(theme::T::TextFaint);
        if (close_hit) close_el = close_el | ftxui::reflect(close_hit->box);
        parts.push_back(close_el);
    }
    parts.push_back(ftxui::text(" "));
    Element el = ftxui::hbox(std::move(parts));
    // 选中：白色边框线；未选中：borderEmpty 占位（不画线，保持布局一致）
    if (selected)
        el = el | ftxui::borderStyled(ftxui::BorderStyle::LIGHT) | ftxui::color(theme::T::Text);
    else
        el = el | ftxui::borderEmpty;
    if (hit) el = el | ftxui::reflect(hit->box);
    return el;
}

/// @brief 后台任务区：名称 + 状态 + 进度条
Element build_background_tasks(const std::vector<TaskLite>& tasks) {
    Elements rows;
    for (const auto& t : tasks) {
        std::string status_icon = t.status == "Running" ? "●" : (t.status == "Failed" ? "✗" : "✓");
        Color status_color = t.status == "Running" ? theme::T::Accent
                                                   : (t.status == "Failed" ? ftxui::Color::RedLight
                                                                           : theme::T::TextDim);
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(status_icon) | ftxui::color(status_color),
            ftxui::text(" "),
            ftxui::flex(ftxui::text(t.name) | ftxui::color(theme::T::TextDim)),
            ftxui::text(t.status) | ftxui::color(theme::T::TextFaint),
        }));
    }
    return ftxui::vbox(std::move(rows));
}

/// @brief 任务调度 tab：任务聚合区 + 上下文信息 + 原侧栏内容（底部）
Element build_tasks_tab(const SidebarTabsModel& tabs, const SidebarModel& sidebar,
                        const ftxui::Element& sub_menu_elem, std::deque<SectionHit>* section_hits) {
    Elements rows;

    // 状态行：● 生成中 / 空闲（模型名在下方聚合信息区「模型」行显示，避免重复）
    rows.push_back(ftxui::hbox({
        ftxui::text("  "),
        ftxui::text(tabs.busy ? std::string(str::kTasksStatusBusy)
                              : std::string(str::kTasksStatusIdle)) |
            ftxui::color(tabs.busy ? theme::T::Accent : theme::T::TextDim),
    }));

    // 当前工具
    rows.push_back(kv_line(str::kTasksTool, tabs.current_tool.empty()
                                                ? str::kDash
                                                : std::string_view(tabs.current_tool)));
    // 步骤进度
    std::string steps = std::to_string(tabs.step_number) + std::string(str::kSlash) +
                        std::to_string(tabs.total_steps);
    rows.push_back(kv_line(str::kTasksSteps, steps));

    // 子 Agent 菜单（可交互：App 侧渲染 Menu 组件，Enter 跳转转录）
    if (!tabs.sub_agents.empty()) {
        rows.push_back(section_title(str::kTasksSubAgents));
        rows.push_back(sub_menu_elem);
    }

    // 后台任务区
    if (!tabs.background_tasks.empty()) {
        rows.push_back(section_title(str::kTasksBackground));
        rows.push_back(build_background_tasks(tabs.background_tasks));
    }

    rows.push_back(ftxui::text(" "));

    // 聚合信息：新会话 / 项目 / 模型 / 上下文 / Token / 缓存 / MCP / TODO
    append_sidebar_info(rows, sidebar, section_hits);

    return ftxui::vbox(std::move(rows));
}

/// @brief 文件 tab：/view 只读查看器（路径栏 + 行号 + 虚拟化滚动）
/// @param file_elem App 侧渲染的组件元素（可交互）；空则回退纯渲染
Element build_files_tab(const FileViewState& file, const ftxui::Element& file_elem) {
    if (file_elem) return file_elem;
    return build_file_viewer(file);
}

/// @brief 变更记录 tab：修改点 Menu + hunk + 目的展开（P5）
/// @param change_elem App 侧渲染的组件元素（可交互）；空则回退纯渲染
Element build_changes_tab(const ChangeViewState& changes, const ftxui::Element& change_elem) {
    if (change_elem) return change_elem;
    return build_change_viewer(changes);
}

/// @brief 项目 tab：项目文件树（常驻；App 侧渲染组件元素，空则回退纯渲染）
Element build_projects_tab(const ProjectTreeState& project, const ftxui::Element& tree_elem) {
    if (tree_elem) return tree_elem;
    return build_project_tree(project);
}

}  // namespace

/// @brief 子 Agent 菜单条目单行标签：`● id · 步骤 N` / `✓ id · 完成` / `✗ id · 失败`
std::string sub_agent_label(const SubAgentLite& a) {
    std::string icon = a.status == "running" ? "●" : (a.status == "failed" ? "✗" : "✓");
    std::string id = a.task_id;
    if (id.size() > 12) id = id.substr(0, 12);  // task_id 为 ASCII，字节截断安全
    std::string status_txt = a.status == "running"
                                 ? (std::string(str::kSubStepLabel) + std::to_string(a.step_number))
                                 : (a.status == "failed" ? std::string(str::kSubStatusFailed)
                                                         : std::string(str::kSubStatusDone));
    return icon + " " + id + " · " + status_txt;
}

Element build_sidebar_tabs(const SidebarTabsModel& tabs, const SidebarModel& sidebar,
                           std::deque<TabHit>* hit_boxes, std::deque<SectionHit>* section_hits,
                           const ftxui::Element& sub_menu_elem,
                           const ftxui::Element& change_viewer_elem,
                           const ftxui::Element& project_tree_elem,
                           const ftxui::Element& file_viewer_elem) {
    if (hit_boxes) hit_boxes->clear();

    // ---- tab 栏（自绘 hbox）：任务调度(常驻) | 项目(常驻) | 变更记录(可关) | 文件(可关) ----
    Elements bar;
    auto push_tab = [&](SidebarTab tab, const std::string& label, bool closable) {
        if (!bar.empty()) bar.push_back(ftxui::text(" "));  // tab 间空格分隔（选中项自带边框线）
        TabHit* hit = nullptr;
        TabHit* close_hit = nullptr;
        if (hit_boxes) {
            hit_boxes->push_back(TabHit{});
            hit_boxes->back().tab = tab;
            hit_boxes->back().close = false;
            hit = &hit_boxes->back();
            if (closable) {
                hit_boxes->push_back(TabHit{});
                hit_boxes->back().tab = tab;
                hit_boxes->back().close = true;
                close_hit = &hit_boxes->back();
            }
        }
        bar.push_back(tab_block(label, tabs.active == tab, closable, hit, close_hit));
    };

    push_tab(SidebarTab::kTasks, std::string(str::kTabTasks), false);
    push_tab(SidebarTab::kProjects, std::string(str::kTabProjects), false);
    if (tabs.changes_open) push_tab(SidebarTab::kChanges, std::string(str::kTabChanges), true);
    if (tabs.file_open) push_tab(SidebarTab::kFiles, std::string(str::kTabFiles), true);

    Element tab_bar = ftxui::hbox(std::move(bar)) | ftxui::xflex;

    // ---- 内容区 ----
    Element content;
    switch (tabs.active) {
        case SidebarTab::kTasks:
            content = build_tasks_tab(tabs, sidebar, sub_menu_elem, section_hits);
            break;
        case SidebarTab::kProjects:
            content = build_projects_tab(tabs.project, project_tree_elem);
            break;
        case SidebarTab::kFiles:
            content = build_files_tab(tabs.file, file_viewer_elem);
            break;
        case SidebarTab::kChanges:
            content = build_changes_tab(tabs.changes, change_viewer_elem);
            break;
        default:
            content = ftxui::emptyElement();
            break;
    }

    return ftxui::vbox({
        ftxui::text(" "),  // 顶部内边距
        ftxui::hbox({
            ftxui::text(" "),  // 左侧内边距（1 格）
            tab_bar,
            ftxui::text(" "),
        }),
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        content | ftxui::yflex,
    });
}

}  // namespace ftxtui
