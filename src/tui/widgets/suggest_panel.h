/**
 * @file suggest_panel.h
 * @brief 输入栏提示面板："/" 命令模式 + "@" 文件模式（纯渲染，不占焦点）
 * @details 输入框上方弹出的候选列表，无自己的搜索框（搜索框即 composer）。
 *          状态（模式/条目/选中项）由调用方（App）持有，本组件只负责渲染。
 *          键盘事件经 composer 转发到 App 的状态机（见 composer.h 回调）。
 */

#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace ftxtui {

/// @brief 提示面板模式
enum class SuggestMode {
    None,     ///< 无面板
    Command,  ///< 行首 "/" → 命令候选
    File,     ///< 行内 "@"（后无空格）→ 文件候选
};

/// @brief 提示面板条目（UI 侧独立数据模型；payload 为调用方数据源下标）
struct SuggestEntry {
    std::string title;     ///< 主标题（如 "/clear"、文件名）
    std::string subtitle;  ///< 副标题（命令描述 / 相对路径），可空
    int payload = 0;       ///< 原始下标（命令 / 文件各自索引）
};

/// @brief 命令条目（从 agent 命令注册表派生；UI 侧数据模型，命令/搜索面板共用）
struct PaletteCommand {
    std::string command;      ///< 实际执行命令，如 "/model"
    std::string title;        ///< 命令名，如 "model"（副标题的降级提示语来源）
    std::string description;  ///< 命令描述（提示语），如 "切换模型"
    std::string keywords;     ///< 额外搜索关键词（中英文/别名），可空
};

/// @brief 从输入行推导提示面板模式与过滤查询（对齐 src/tui bottom_bar 语义）
/// @param line 当前输入文本
/// @param[out] query 过滤查询（不含 "/" 或 "@" 前缀）
/// @return 推导出的模式
SuggestMode parse_suggest_query(const std::string& line, std::string& query);

/// @brief 按子串过滤命令条目（大小写不敏感，匹配 command+title+keywords）
/// @return 命中的原始下标列表（保持原顺序）
std::vector<size_t> filter_commands(const std::vector<std::string>& commands,
                                    const std::string& query);

/// @brief 命令面板确认：把完整命令「追加」到输入行
/// @param line 当前输入行（可能含 "/" 之前已输入的内容，如 "test /skill"）
/// @param full 选中的完整命令（含前导 "/"，如 "/skill"）
/// @return 追加后的输入行：保留最后一个 "/"（非 @路径内）之前的内容，
///         从该 "/" 起替换为 full + " "；无 "/" 时直接追加 full + " "
std::string apply_command_suggest(const std::string& line, const std::string& full);

/// @brief 渲染输入框上方提示面板
/// @param mode 面板模式（None 返回空元素）
/// @param entries 已过滤的候选列表
/// @param selected 选中项下标（-1 = 无选中）
/// @param file_ready 文件索引是否就绪（File 模式未就绪时显示「构建中」）
/// @param hit_boxes 非空时记录每条候选行渲染后的屏幕 box（鼠标点击命中用；
///                  deque 保证 reflect 持有的 Box& 地址稳定）
ftxui::Element render_suggest_panel(SuggestMode mode, const std::vector<SuggestEntry>& entries,
                                    int selected, bool file_ready,
                                    std::deque<ftxui::Box>* hit_boxes = nullptr);

}  // namespace ftxtui