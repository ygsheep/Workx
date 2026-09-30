/**
 * @file composer.h
 * @brief 底部输入组件（ftxui::Input + 快捷键处理）
 * @details Enter 提交；Tab 切换工作模式（标准/极简/计划）；Shift+Tab 切换权限；
 *          Ctrl+T 切换工作模式（与 Tab 同）；Ctrl+P 命令面板。
 *          MVP 为单行输入（ftxui Input 原生不支持多行 textarea），后续可用
 *          Renderer 自绘多行编辑器替代。
 */

#pragma once

#include <functional>
#include <string>

#include <ftxui/component/component.hpp>

namespace ftxtui {

/// @brief composer 回调与状态
struct ComposerOptions {
    std::string* buffer = nullptr;  ///< 输入缓冲（Input 直连）
    size_t* cursor = nullptr;       ///< 光标位置（外部持有，可为空）
    std::function<void(const std::string&)> on_submit;  ///< 提交（Enter）
    /// @brief 提交并请求立即冲刷（Ctrl+Enter）：入队 + 下个工具轮边界注入
    std::function<void(const std::string&)> on_submit_ctrl;
    std::function<void()> on_perm_toggle;      ///< 权限切换（Shift+Tab）
    std::function<void()> on_mode_toggle;      ///< 工作模式切换（Tab / Ctrl+T）
    std::function<void()> on_toggle_thinking;  ///< 思考视图（Ctrl+O）
    /// @brief 打开系统默认编辑器编辑当前输入（Ctrl+G）：同步输入到 Prompt 文件，
    ///        编辑结束后把文件内容同步回输入框。nullptr = 不启用。
    std::function<void()> on_edit;

    // ---- 输入栏提示面板（App 侧状态机；返回 true = 已消费）----
    std::function<bool()> suggest_active;   ///< 面板是否激活
    std::function<void(int)> suggest_move;  ///< 移动选择（±1；Tab 传 +1，App 循环）
    std::function<bool()> suggest_enter;    ///< Enter 确认面板动作
    std::function<bool()> suggest_enter_insert;  ///< Ctrl+Enter：插入引用（@路径）
    std::function<void()> suggest_cancel;        ///< Esc 关闭面板
    std::function<void()> suggest_refresh;       ///< 字符 / 退格后刷新过滤

    // ---- 输入历史（上下箭头浏览；返回 true = 已消费）----
    std::function<bool()> on_history_prev;  ///< ArrowUp 首行：上一条
    std::function<bool()> on_history_next;  ///< ArrowDown 末行：下一条
};

/// @brief 构建 composer 组件
ftxui::Component make_composer(ComposerOptions& opt);

}  // namespace ftxtui