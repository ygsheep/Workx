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

/// @brief 进度文案后缀：完全没有任何隔离
constexpr std::string_view kNoIsolationNote = " - command runs WITHOUT OS-level isolation";

/// @brief 进度文案后缀：无 FS/网络隔离，但有进程级约束（#84 方案 B）
constexpr std::string_view kProcessIsolationNote =
    " - no FS/network isolation; process isolation: job-object (process count and memory "
    "capped, process tree killed on exit)";

/// @brief 选择隔离说明后缀
/// @details 有进程级隔离时收窄措辞：此时命令确实受进程树与资源约束，
///          笼统说"完全未经隔离"会与事实不符，反而削弱这条安全提示的可信度。
std::string isolation_note(const process::sandbox::WrappedCommand& wrapped) {
    if (wrapped.isolation.has_value() && wrapped.isolation->is_meaningful()) {
        return std::string{kProcessIsolationNote};
    }
    return std::string{kNoIsolationNote};
}

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
                               isolation_note(wrapped));
        case SandboxState::Disabled:
            return std::format("Sandbox: disabled (dangerously_disable_sandbox){}",
                               isolation_note(wrapped));
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

void SandboxVisibility::report_isolation(process::IsolationOutcome outcome,
                                         const std::string& tool_name, const ToolContext& ctx) {
    if (outcome != process::IsolationOutcome::Failed) {
        return;
    }

    ctx.report_progress("Sandbox: process isolation UNAVAILABLE - job object not applied");

    // 与"无后端"的降级是两件独立的事（前者是 FS/网络策略缺席，后者是进程级约束施加失败），
    // 因此用独立的去重标志，互不吞并。
    if (isolation_failed_logged_.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    audit::AuditLogger::instance().log_security(
        audit::EventType::SecuritySandboxDegraded,
        std::format("platform={} requested_level=restrictive actual_backend=none "
                    "reason=job_object_unavailable",
                    platform_name()),
        ctx.session_id, tool_name);
}

}  // namespace agent::tool::shell_common
