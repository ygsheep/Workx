/**
 * @file sandbox_visibility.h
 * @brief 沙箱运行时状态的可见化与审计留痕（#84）
 * @details Windows 上没有可用的进程级沙箱后端，SandboxAdapter::wrap_command() 会静默降级为
 *          "原命令直通"（degraded=true, was_wrapped=false）。而 BashTool / PowerShellTool 的
 *          上报此前被 `if (wrapped.was_wrapped)` 门控 —— 降级路径永远进不去，于是出现
 *          "用户以为开了 restrictive，实际命令完全未经隔离且毫无提示"的错觉。
 *
 *          本模块把这件事从静默变成显式：
 *          - 每次执行都上报进度文案（active / degraded / disabled 三态）
 *          - 降级 / 显式关闭各在实例生命周期内只写一条审计事件，避免逐条命令刷爆 audit.jsonl
 *
 *          纯函数（状态判定 / 文案 / 详情）与有状态上报器分离：前者可直接单测，
 *          后者便于在测试中构造实例并断言"只留痕一次"。
 *
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <atomic>
#include <string>

#include "core/process/process_isolation.h"
#include "core/process/sandbox/sandbox_adapter.h"

namespace agent::tool {
struct ToolContext;
}

namespace agent::tool::shell_common {

/// @brief 沙箱运行时状态
enum class SandboxState {
    Active,    ///< 后端生效并实际包装了命令（seatbelt / bubblewrap）
    Degraded,  ///< 后端不可用，命令未经隔离直接执行
    Disabled,  ///< 调用方显式关闭沙箱（dangerously_disable_sandbox）
};

/// @brief 由包装结果与调用方开关判定沙箱状态（纯函数）
/// @param wrapped SandboxAdapter::wrap_command() 的返回值
/// @param disable_sandbox 调用方是否显式关闭沙箱
/// @note 未包装、未标记降级、也非显式关闭时一律按 Degraded 处理 ——
///       宁可误报"无隔离"，也不要漏报"看起来有隔离其实没有"。
SandboxState classify_sandbox(const process::sandbox::WrappedCommand& wrapped,
                              bool disable_sandbox) noexcept;

/// @brief 生成进度文案（纯函数）
/// @details Active 保持既有格式 `Sandbox: active (backend: <name>)`；
///          Degraded / Disabled 会明确写出隔离的实际状态。有进程级隔离
///          （#84 方案 B 的 Job Object）时措辞收窄为"无文件系统/网络隔离"，
///          否则说"完全未经隔离"会与事实不符。
std::string sandbox_progress_message(SandboxState state,
                                     const process::sandbox::WrappedCommand& wrapped);

/// @brief 生成审计事件详情（纯函数，key=value 便于 jq 过滤）
/// @details 记录 platform / requested_level（用户以为开了什么）/ actual_backend（实际拿到什么）
///          / reason，便于事后归因。
std::string sandbox_audit_detail(SandboxState state,
                                 const process::sandbox::WrappedCommand& wrapped);

/// @brief 沙箱状态上报器（#84 方案 A）
/// @details 持有"是否已写过审计"的实例级状态。BashTool / PowerShellTool 各持有一个，
///          因此同一工具实例内降级只留痕一次（进度文案仍逐次上报）。
class SandboxVisibility {
   public:
    /// @brief 上报一次沙箱状态（进度回调 + 审计留痕）
    /// @param wrapped 包装结果
    /// @param disable_sandbox 调用方是否显式关闭沙箱
    /// @param tool_name 审计事件中的工具名（如 "Bash"）
    /// @param ctx 工具上下文（提供 progress_callback 与 session_id）
    void report(const process::sandbox::WrappedCommand& wrapped, bool disable_sandbox,
                const std::string& tool_name, const ToolContext& ctx);

    /// @brief 上报进程级隔离（Job Object）的实际应用结果（#84 方案 B）
    /// @param outcome process::exec() 写入 ExecOutput::isolation 的结果
    /// @param tool_name 审计事件中的工具名（如 "Bash"）
    /// @param ctx 工具上下文
    /// @details 只在 Failed 时产生输出：一条进度警告 + 一条审计（实例内只写一次）。
    ///          Applied 不额外提示 —— 命令前那条进度文案里已含隔离描述。
    void report_isolation(process::IsolationOutcome outcome, const std::string& tool_name,
                          const ToolContext& ctx);

   private:
    std::atomic<bool> degraded_logged_{false};  ///< 降级审计是否已写
    std::atomic<bool> disabled_logged_{false};  ///< 关闭沙箱审计是否已写
    /// 进程级隔离未能施加的审计是否已写（#84 方案 B，与"无后端"是两回事，故独立去重）
    std::atomic<bool> isolation_failed_logged_{false};
};

}  // namespace agent::tool::shell_common
