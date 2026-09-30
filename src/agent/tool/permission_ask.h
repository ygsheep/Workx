/**
 * @file permission_ask.h
 * @brief 权限确认工具（#36：危险操作的 AskUser 确认通道）
 * @details 复用 AskUserRequestEvent 事件通道向宿主（TUI）请求用户确认。
 *          宿主无关：event_bus 不可用时 fail-closed（拒绝）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>

#include "agent/tool/context.h"

namespace agent::tool {

/// @brief 请求用户确认（阻塞式，最多等待 timeout_ms）
/// @param ctx 工具上下文（event_bus 可用才能提问）
/// @param question 展示给用户的问题
/// @param timeout_ms 等待时长（默认 60s；超时视为拒绝）
/// @return true=用户允许；false=拒绝/超时/无确认通道
bool ask_user_confirm(const ToolContext& ctx, const std::string& question, int timeout_ms = 60000);

/// @brief 是否计划（只读）模式
bool is_plan_mode(PermissionMode mode) noexcept;

/// @brief 是否完全放行模式
bool is_bypass_mode(PermissionMode mode) noexcept;

/// @brief 当前模式下应禁止写文件（Plan → true；Bypass → false）
bool deny_write_by_mode(PermissionMode mode) noexcept;

/// @brief 当前模式下应禁止执行命令（Plan → true；Bypass → false）
bool deny_execute_by_mode(PermissionMode mode) noexcept;

/// @brief 危险命令检测（#36）
/// @details 命令含破坏性模式（rm -rf / mkfs / shutdown / 管道到 shell 等）
///          时返回 true，Default 模式下应走 ask_user_confirm 确认。
///          大小写不敏感，子串匹配。
bool is_dangerous_command(const std::string& command) noexcept;

}  // namespace agent::tool