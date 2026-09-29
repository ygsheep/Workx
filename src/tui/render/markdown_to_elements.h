/**
 * @file markdown_to_elements.h
 * @brief Markdown → ftxui::Element 渲染适配（复用 markdown_renderer 的解析思路）
 * @details 原 src/tui/render/markdown_renderer 输出 ANSI 字符串；本实现把同一套
 *          解析结构迁移为 ftxui::Element（尺寸自适应、无 ANSI）。与 src/tui 零耦合。
 *          当前覆盖：代码块 / 标题 / 分隔线 / 列表 / 段落 / 行内（粗体斜体删除线代码）。
 *          表格降级为纯文本（后续增强）。
 */

#pragma once

#include <cstddef>
#include <deque>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "vm/message_node.h"

namespace ftxtui {

/// @brief 渲染整段 Markdown 为纵向元素
/// @param compact 紧凑模式：普通段落每物理行占 1 行（不做 ×2 行距），
///                供用户消息使用——块高即文本行数，不产生段落尾随空行，
///                与转录区统一间距叠加后不会出现"多一行"。
ftxui::Element build_markdown(std::string_view text, int width, bool compact = false);

/// @brief 文件路径扩展名 → 语法高亮语言标签；未知返回空
/// @details 供工具卡 / 文件查看器（/view）复用同一套扩展名映射
std::string lang_from_path(const std::string& path);

/// @brief 估算 Markdown 渲染行数（与 build_markdown 布局逐行一致；A3 单一布局源）
/// @param width 正文折行的单行最大显示列宽；代码行/表格不折行
/// @param compact 与 build_markdown 的 compact 对应，普通段落按每物理行 1 行计数
int estimate_markdown_height(std::string_view text, int width, bool compact = false);

/// @brief 估算整条消息渲染行数（与 build_message 布局逐行一致；A3 单一布局源）
/// @details 供转录滚动/视口高度估算使用，消除 app 侧手工复刻的布局漂移。
///          正文行数随 width 变化，须与 build_message 传入相同宽度。
int estimate_message_height(const MessageNode& msg, int width);

/// @brief 渲染单行行内文本（含粗体/斜体/删除线/代码）为横向元素
ftxui::Element build_inline_line(std::string_view line);

/// @brief 折叠卡片渲染后的屏幕位置（reflect 记录，供点击命中）
struct CardHit {
    ftxui::Box box;     ///< 渲染后的屏幕坐标
    int msg_idx = -1;   ///< 消息索引（渲染层不知，由调用方回填）
    int tool_idx = -1;  ///< -1 = 思考卡片；>=0 = 工具块索引
    int button = -1;    ///< >=0 = 消息操作按钮（0=复制，1=重试）；-1 = 非按钮
    /// @brief 面包屑导航目标：-1 = 非导航；0 = 主会话；1 = 子 Agent 记录
    int nav_target = -1;
    /// @brief 第二层（子 Agent 记录）卡片命中的步骤索引（-1 = 思考卡；>=0 = steps 下标）
    int sub_step = -1;
};

/// @brief 把一条消息渲染为元素（用户/助手/错误 + 思考折叠 + 工具块）
/// @param msg 消息节点（只读；展开/折叠状态就地切换，由调用方负责重绘）
/// @param anim_frame 动画帧号（思考中旋转动画）
/// @param card_hits 非空时记录每张折叠卡片渲染后的屏幕 box（点击命中用）
ftxui::Element build_message(const MessageNode& msg, int width, std::size_t anim_frame = 0,
                             std::deque<CardHit>* card_hits = nullptr);

}  // namespace ftxtui