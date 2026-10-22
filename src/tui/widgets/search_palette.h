/**
 * @file search_palette.h
 * @brief 全局搜索面板（Ctrl+P）与统一悬浮选择面板（/resume /model /供应商）
 * @details 悬浮居中窗口，顶部搜索框实时过滤，条目混排为单一列表，
 *          按匹配度排序（标题前缀命中优先），每行右侧显示类别标签，
 *          使用中条目（active）行首以 ● 标记。
 *          键盘：↑↓ / Ctrl+N / Ctrl+P 移动，Tab 向下循环选择，
 *          Enter 执行，Esc 先清空搜索再关闭。
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

namespace ftxtui {

/// @brief 条目类别（决定行右侧标签与选中动作；未知类别不显示标签）
enum class SearchCategory {
    Feature,   ///< 功能（注册命令）
    File,      ///< 文件名
    Session,   ///< 会话记录
    Setting,   ///< 设置
    Model,     ///< 模型（/model 面板）
    Provider,  ///< 供应商（切换面板）
};

/// @brief 面板条目（UI 侧独立数据模型）
struct SearchEntry {
    SearchCategory category = SearchCategory::Feature;  ///< 条目类别（默认 Feature）
    std::string title;  ///< 主标题（命令名 / 文件名 / 会话标题 / 设置名 / 模型名）
    std::string subtitle;  ///< 副标题（描述 / 路径 / URL），可空
    std::string keywords;  ///< 额外搜索词（中英文别名），可空
    int payload = 0;       ///< 调用方数据源下标
    bool active = false;   ///< 使用中标记（行首 ●，如当前模型/供应商）
};

/// @brief 按子串过滤条目（大小写不敏感；标题前缀命中排前，其余保持原顺序）
/// @return 命中的原始下标列表
std::vector<int> filter_search_entries(const std::vector<SearchEntry>& entries,
                                       const std::string& query);

/// @brief 构建搜索/选择面板（内部自带搜索 + 过滤，打开后自动聚焦）
/// @param entries 全部条目（调用方持有：打开前装配，加载完成后更新；
///                面板每帧重新过滤，打开边沿自动清空搜索框）
/// @param on_select 选中回调（Enter 执行；参数为 entries 的原始下标）
/// @param open 开关引用（Esc / Enter 关闭时置 false）
/// @param on_close 关闭回调（用于恢复焦点等；可为空）
/// @param title 面板标题（显示在搜索框上方，空则不显示）
/// @param restrict_default 为 true 且搜索框为空时，仅保留「会话记录 / 设置」两类，
///                          供全局聚合面板（Ctrl+P）默认去噪；输入后恢复全类搜索。
ftxui::Component make_search_palette(std::vector<SearchEntry>& entries,
                                     std::function<void(int)> on_select, bool& open,
                                     std::function<void()> on_close = nullptr,
                                     std::string title = "", bool restrict_default = false);

}  // namespace ftxtui