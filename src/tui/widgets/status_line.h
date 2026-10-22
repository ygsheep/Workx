/**
 * @file status_line.h
 * @brief composer 上方单行状态行（模式 / 权限 / 忙碌动画）
 */

#pragma once

#include <cstddef>
#include <string>

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 构建状态行元素（内嵌于输入区下方，单行）
/// @param model 模型名
/// @param mode 工作模式标签（"standard" / "plan" / "minimal"）
/// @param permission 权限标签（"" / "bypass"，计划模式时忽略）
/// @param busy 是否生成中（驱动前置思考动画）
/// @param anim_frame 动画帧号（busy 时旋转；空闲显示静态点）
/// @param todo_done 已完成待办数（#24：>0 时显示 ✓ X/Y）
/// @param todo_total 待办总数
ftxui::Element build_status_line(const std::string& model, const std::string& mode,
                                 const std::string& permission, bool busy, std::size_t anim_frame,
                                 int todo_done = 0, int todo_total = 0);

}  // namespace ftxtui
