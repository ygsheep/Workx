/**
 * @file change_viewer.h
 * @brief 变更记录视图（修改点 Menu + hunk + 目的展开）
 * @details 可聚焦组件：↑↓ 选择修改点、e 展开/收起目的、Enter 跳转文件 tab。
 *          纯渲染函数 build_change_viewer 供无头测试与未聚焦时回退渲染。
 */

#pragma once

#include <deque>
#include <functional>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 修改点行命中区（鼠标点击选中用）
struct ChangeHit {
    int index = -1;  ///< 修改点下标（changes.changes 下标）
    ftxui::Box box;  ///< 命中区屏幕 box
};

/// @brief 构建变更记录 tab 组件（修改点 Menu + hunk + 目的展开）
/// @param changes 变更记录状态（App 持有）
/// @param on_jump 选中修改点 Enter 回调（App 跳转文件 tab）
ftxui::Component make_change_viewer(ChangeViewState* changes, std::function<void()> on_jump);

/// @brief 变更记录视图渲染（纯元素）
/// @param changes 变更记录状态
/// @param hits 非空时记录各修改点行命中区（鼠标点击用）
ftxui::Element build_change_viewer(const ChangeViewState& changes,
                                   std::deque<ChangeHit>* hits = nullptr);

}  // namespace ftxtui
