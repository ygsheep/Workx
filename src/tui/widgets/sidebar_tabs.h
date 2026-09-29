/**
 * @file sidebar_tabs.h
 * @brief 侧边栏 tab 容器（任务调度 | 变更记录 | 文件）
 * @details 自绘 tab 栏（非纯 Menu，因每 tab 需独立 ✕ 关闭按钮）+ 内容区。
 *          纯渲染组件：状态由调用方（App）持有，本组件只负责渲染与命中区记录。
 */

#pragma once

#include <deque>

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"
#include "widgets/sidebar.h"

namespace ftxtui {

/// @brief tab 栏命中区（区分「切换」与「关闭」）
struct TabHit {
    SidebarTab tab = SidebarTab::kTasks;  ///< 命中的 tab
    bool close = false;                   ///< true=命中 ✕ 关闭按钮
    ftxui::Box box;                       ///< 命中区屏幕 box
};

/// @brief 子 Agent 菜单条目单行标签（任务调度 tab 可交互 Menu 用）
/// @details 格式：`● id · 步骤 N` / `✓ id · 完成` / `✗ id · 失败`；id 为 ASCII 字节截断
std::string sub_agent_label(const SubAgentLite& a);

/// @brief 构建侧边栏 tab 容器（tab 栏 + 内容区）
/// @param tabs tab 状态模型（active / 打开标志 / 任务调度数据）
/// @param sidebar 原侧栏数据（任务调度 tab 底部）
/// @param hit_boxes 非空时记录各 tab 命中区（鼠标点击用；deque 保证 reflect 地址稳定）
/// @param section_hits 非空时记录侧栏可折叠区块标题命中区（MCP/TODO 点击用）
/// @param sub_menu_elem 子 Agent 菜单元素（App 侧渲染 Menu 组件；空 = 无子 Agent）
/// @param change_viewer_elem 变更记录组件渲染元素（App 侧渲染；空 = 回退纯渲染）
/// @param project_tree_elem 项目文件树组件渲染元素（App 侧渲染；空 = 回退纯渲染）
/// @param file_viewer_elem 文件查看器组件渲染元素（App 侧渲染；空 = 回退纯渲染）
ftxui::Element build_sidebar_tabs(const SidebarTabsModel& tabs, const SidebarModel& sidebar,
                                  std::deque<TabHit>* hit_boxes = nullptr,
                                  std::deque<SectionHit>* section_hits = nullptr,
                                  const ftxui::Element& sub_menu_elem = ftxui::emptyElement(),
                                  const ftxui::Element& change_viewer_elem = ftxui::Element(),
                                  const ftxui::Element& project_tree_elem = ftxui::Element(),
                                  const ftxui::Element& file_viewer_elem = ftxui::Element());

}  // namespace ftxtui
