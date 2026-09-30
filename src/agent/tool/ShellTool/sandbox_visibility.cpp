/**
 * @file sandbox_visibility.cpp
 * @brief 沙箱运行时状态的可见化与审计留痕（#84）实现
 * @version 1.0.0
 * @date 2026-10
 */

#include "agent/tool/ShellTool/sandbox_visibility.h"

#include <format>
#include <string_view>

#include "agent/audit/audit_logger.h"
#include "agent/tool/context.h"

namespace agent::tool::shell_common {

namespace {

/// @brief 当前平台标识（写入审计事件，便于事后归因）
constexpr std::string_view platform_name() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

/// @brief 后端名（宽松配置下 passthrough 不带后端名，回落 "none"）
std::string_view backend_name(const process::sandbox::WrappedCommand& wrapped) noexcept {
    return wrapped.backend_name.empty() ? std::string_view{"none"}
                                        : std::string_view{wrapped.backend_name};
}

/// @brief 进度文案后缀：明确写出"未经 OS 级隔离"
constexpr std::string_view kNoIsolationNote = " - command runs WITHOUT OS-level isolation";

}  // namespace

SandboxState classify_sandbox(const process::sandbox::WrappedCommand& wrapped,
                              bool disable_sandbox) noexcept {
    if (disable_sandbox) {
        return SandboxState::Disabled;
    }
    if (wrapped.was_wrapped && !wrapped.degraded) {
        return SandboxState::Active;
    }
    return SandboxState::Degraded;
}

std::string sandbox_progress_message(SandboxState state,
                                     const process::sandbox::WrappedCommand& wrapped) {
    switch (state) {
        case SandboxState::Active:
            return std::format("Sandbox: active (backend: {})", backend_name(wrapped));
        case SandboxState::Degraded:
            return std::format("Sandbox: degraded (backend: {}){}", backend_name(wrapped),
                               kNoIsolationNote);
        case SandboxState::Disabled:
            return std::format("Sandbox: disabled (dangerously_disable_sandbox){}",
                               kNoIsolationNote);
    }
    return std::string{"Sandbox: unknown"};
}

std::string sandbox_audit_detail(SandboxState state,
                                 const process::sandbox::WrappedCommand& wrapped) {
    const bool disabled = (state == SandboxState::Disabled);
    return std::format("platform={} requested_level={} actual_backend={} reason={}",
                       platform_name(), disabled ? "permissive" : "restrictive",
                       backend_name(wrapped),
                       disabled ? "dangerously_disable_sandbox" : "backend_unavailable");
}

void SandboxVisibility::report(const process::sandbox::WrappedCommand& wrapped,
                               bool disable_sandbox, const std::string& tool_name,
                               const ToolContext& ctx) {
    const SandboxState state = classify_sandbox(wrapped, disable_sandbox);

    // 进度文案逐次上报：命令流的每一步都该看到真实的隔离状态
    ctx.report_progress(sandbox_progress_message(state, wrapped));

    if (state == SandboxState::Active) {
        return;
    }

    // 审计留痕（降级 / 显式关闭各只写一次，避免逐条命令刷爆 audit.jsonl）
    std::atomic<bool>& logged =
        (state == SandboxState::Disabled) ? disabled_logged_ : degraded_logged_;
    if (logged.exchange(true, std::memory_order_relaxed)) {
        return;
    }

    const audit::EventType type = (state == SandboxState::Disabled)
                                      ? audit::EventType::SecuritySandboxDisabled
                                      : audit::EventType::SecuritySandboxDegraded;
    audit::AuditLogger::instance().log_security(type, sandbox_audit_detail(state, wrapped),
                                                ctx.session_id, tool_name);
}

}  // namespace agent::tool::shell_common
