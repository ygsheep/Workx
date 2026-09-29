/**
 * @file theme.h
 * @brief 全站统一主题：颜色 + 常用间距（FTXUI 实验 TUI）
 * @details
 *   背景以深灰色为主（#1e1e1e），故主体文字统一用米白（#F5F5F0），
 *   避免「灰底灰字」的低对比问题。次级/弱提示用可读的中灰。
 *   所有文件应引用本主题常量，不要散落硬编码的 RGB / Color::前缀。
 */

#pragma once

#include <ftxui/dom/elements.hpp>

namespace ftxtui {

namespace theme {

// ============================================================================
// 颜色
// ============================================================================
struct T {
    inline static const ftxui::Color Canvas =
        ftxui::Color::RGB(0x00, 0x00, 0x00);  // 画布/最外层背景
    inline static const ftxui::Color Panel =
        ftxui::Color::RGB(0x1e, 0x1e, 0x1e);  // 面板/代码块/输入区/侧栏
    inline static const ftxui::Color Surface =
        ftxui::Color::RGB(0x26, 0x26, 0x26);  // 卡片/转录区（比 Panel 略高）
    inline static const ftxui::Color Selection = ftxui::Color::RGB(0x2d, 0x3a, 0x55);  // 选中项背景
    inline static const ftxui::Color Accent =
        ftxui::Color::RGB(0x5c, 0x9c, 0xf5);  // 强调：光标/边框线/搜索图标
    inline static const ftxui::Color Text = ftxui::Color::RGB(0xF5, 0xF5, 0xF0);  // 主文字（米白）
    inline static const ftxui::Color TextDim =
        ftxui::Color::RGB(0xC8, 0xC8, 0xC8);  // 次级文字（可读灰）
    inline static const ftxui::Color TextFaint =
        ftxui::Color::RGB(0x8E, 0x8E, 0x8E);  // 弱提示/时间戳/边框
    inline static const ftxui::Color DiffAdd =
        ftxui::Color::RGB(0x6a, 0xc4, 0x6a);  // diff 新增/修改行（绿）
    inline static const ftxui::Color DiffDel =
        ftxui::Color::RGB(0xE0, 0x5A, 0x5A);  // diff 删除/错误（红）
};

// ============================================================================
// 常用间距 / 背景装饰（统一"上下边距、左右两空格"之类效果）
// ============================================================================

/// @brief 上下各一空行
inline ftxui::Element vPad(ftxui::Element e) {
    return ftxui::vbox({ftxui::text(" "), e, ftxui::text(" ")});
}

/// @brief 左右各两个空格
inline ftxui::Element hPad(ftxui::Element e) {
    return ftxui::hbox({ftxui::text("  "), e | ftxui::flex, ftxui::text("  ")});
}

/// @brief 四周留白：上下一空行 + 左右两空格
inline ftxui::Element pad(ftxui::Element e) { return vPad(hPad(e)); }

/// @brief 施加面板背景色（Panel）
inline ftxui::Element panel(ftxui::Element e) { return e | ftxui::bgcolor(T::Panel); }

}  // namespace theme
}  // namespace ftxtui