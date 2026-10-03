/**
 * @file command_policy.h
 * @brief #85：命令拦截白名单严格档（Strict permission mode 的判定核心）
 * @details 严格档下**默认拒绝**，仅放行显式声明的命令前缀；其余命令走用户确认
 *          （headless 无确认通道 → `ask_user_confirm()` fail-closed 即拒绝）。
 *
 * @par 为什么不能再用子串匹配
 * `is_dangerous_command()` 是 `std::string::find` 的黑名单，`rm -r -f`、
 * `rm --recursive --force`、`find . -delete`、`base64 -d | sh` 都能绕过。
 * 本模块改为**词法判定**：先按命令分隔符切分子命令，再逐条做「可执行名 + 前导参数」
 * 的前缀白名单匹配。
 *
 * @par 覆盖的绕过手法（每项都有对应用例）
 * - 复合命令：`a; b`、`a && b`、`a || b`、`a | b`、换行 —— 逐条判定，**全部**命中才放行
 * - 重定向与命令替换：`>`、`>>`、`<`、`$(...)`、反引号 —— 一律不放行
 * - 动态命令名：`$CMD ...`、`${CMD} ...` —— 一律不放行
 * - shell / 提权包装器：`sh -c`、`bash -c`、`powershell -Command`、`eval`、`sudo`、
 *   `xargs` —— 静态无法判定载荷，一律不放行
 * - 大小写与扩展名：`GIT.EXE STATUS` 与 `git status` 同判
 *
 * @par 刻意不做的事（避免过度承诺）
 * 本模块**不宣称"无法绕过"**。它是"覆盖常见手法 + 审计留痕"的最小可用实现；
 * 真正的进程级隔离是 #113 的范畴（Windows 上本轮走命令级前置拦截，见
 * `.workbuddy/plans/spike-113-windows-isolation.md`）。
 * 亦不做变量展开（`$PATH` 等一律视为动态），那是 shell 解析器的深水区。
 *
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agent/tool/context.h"

namespace agent::tool {

/// @brief 严格档下的命令判定结果
enum class StrictVerdict {
    Allow,  ///< 命中白名单，直接放行
    Ask,    ///< 未命中白名单，需用户确认（无确认通道即拒绝）
    Deny,   ///< 命中风险门禁（破坏性/SSRF/env 泄露）或无法解析，直接拒绝
};

/// @brief 判定结果 + 人类可读原因（进审计 `decision_reason` 与模型可见的错误文案）
struct StrictDecision {
    StrictVerdict verdict = StrictVerdict::Ask;  ///< 判定
    std::string reason;                          ///< 原因（空表示无需说明）
};

/// @brief StrictVerdict 的可读名（审计落盘用）
std::string_view strict_verdict_name(StrictVerdict v) noexcept;

/// @brief 内置白名单（token 前缀；entry 是子命令 token 序列的**前缀**即命中）
/// @details 例如 `{"python", "-m", "pytest"}` 放行 `python -m pytest -x`，
///          但不放行 `python -m pip install x`（后者应走确认）。
///          返回的引用指向静态表，供测试与文档核对，不可修改。
const std::vector<std::vector<std::string>>& strict_command_whitelist() noexcept;

/// @brief 严格档判定入口：风险门禁 + 白名单
/// @param command 待判定的原始命令（Bash 或 PowerShell 均可；两者共用同一张表）
/// @return Allow / Ask / Deny + 原因
/// @details 顺序：
///   1. `detect_shell_risk()` 命中（破坏性 / SSRF / env 泄露）→ **Deny，不询问**
///      （严格档下不该问用户"要不要 rm -rf /"）
///   2. 切分子命令，逐条过白名单；任一条 Ask → 整体 Ask
///   3. 全部命中 → Allow
StrictDecision evaluate_strict_command(std::string_view command);

/// @brief 严格档下的强制处置结果
struct StrictEnforceResult {
    bool allowed = false;  ///< 是否放行
    std::string reason;    ///< 拒绝时的原因（放行时为空）
};

/// @brief 严格档统一处置：判定 → 审计 → 按需询问
/// @param command 待执行命令
/// @param ctx 工具上下文（审计用 session_id/request_id；询问用 event_bus）
/// @param tool_name 工具名（审计字段）
/// @details BashTool / PowerShellTool 共用，避免两处漂移。
///          **放行与拦截都进审计**（`ToolPermissionDecision` + decision=allow/ask/deny）。
///          Ask 档实际是否放行取决于 `ask_user_confirm()`：
///          headless 无确认通道时它 fail-closed，等价于拒绝 —— 这正是严格档
///          在无人值守场景下的价值所在。
StrictEnforceResult enforce_strict_command(std::string_view command, const ToolContext& ctx,
                                           std::string_view tool_name);

}  // namespace agent::tool
