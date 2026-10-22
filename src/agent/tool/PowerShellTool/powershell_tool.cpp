/**
 * @file powershell_tool.cpp
 * @brief PowerShellTool 实现
 * @details Windows PowerShell 命令执行工具的具体实现。
 *          结构对齐 BashTool，复用 shell_tool_common 的辅助函数。
 * @version 1.1.0
 * @date 2026-07
 */

#include "agent/tool/PowerShellTool/powershell_tool.h"

#include "agent/tool/ShellTool/shell_tool_common.h"
#include "agent/tool/background_task_tracker.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/secret_scanner.h"
#include "agent/tool/command_policy.h"
#include "agent/tool/shell_guard.h"
#include "agent/audit/audit_logger.h"
#include "core/process/subprocess.h"
#include "core/process/exec_output.h"
#include "core/process/sandbox/sandbox_adapter.h"
#include "core/process/sandbox/sandbox_config.h"
#include "core/process/sandbox/sandbox_detector.h"
#include "core/task/task_manager.h"
#include "core/utils/error.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>

using namespace agent::tool::shell_common;
using namespace agent::audit;

namespace agent::tool {

namespace {

/// 后台任务输出注册表容量上限
constexpr size_t kMaxRegistryEntries = 50;
/// 后台任务名唯一 id 计数器
std::atomic<size_t> s_task_id_counter{0};

/// @brief 后台任务输出注册表（PowerShell 专属）
/// @details 与 BashOutputRegistry 结构一致，task_name 前缀用 "ps:" 区分
class PSOutputRegistry {
   public:
    static PSOutputRegistry& instance() {
        static PSOutputRegistry inst;
        return inst;
    }

    void store(const std::string& task_name, const process::ExecOutput& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs[task_name] = out;
        if (m_outputs.size() > kMaxRegistryEntries) {
            m_outputs.erase(m_outputs.begin());
        }
    }

    std::optional<process::ExecOutput> get(const std::string& task_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outputs.find(task_name);
        if (it == m_outputs.end()) return std::nullopt;
        return it->second;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs.clear();
    }

   private:
    PSOutputRegistry() = default;
    mutable std::mutex m_mutex;
    std::map<std::string, process::ExecOutput> m_outputs;
};

/// PowerShell shell 路径与参数
/// @details -NoProfile：跳过用户 profile 加载，保证环境干净
///          -NonInteractive：禁止交互式提示（Read-Host 等），避免阻塞
///          -Command：执行后续字符串作为命令
constexpr const char* kShell = "powershell.exe";
constexpr const char* kNoProfileFlag = "-NoProfile";
constexpr const char* kNonInteractiveFlag = "-NonInteractive";
constexpr const char* kCommandFlag = "-Command";

}  // namespace

// ============================================================
// 元信息
// ============================================================

const std::string& PowerShellTool::name() const {
    static const std::string n{"PowerShell"};
    return n;
}

const std::string& PowerShellTool::description() const {
    static const std::string d{
        "Executes a PowerShell command on Windows and returns stdout, stderr, and exit code. "
        "Supports timeout, working directory, and optional background execution."};
    return d;
}

const std::string& PowerShellTool::prompt() const {
    static const std::string p{
        "Executes a PowerShell command on the user's Windows machine.\n\n"
        "## When to use\n"
        "- Windows system administration (Get-Process, Get-Service, Set-Location)\n"
        "- Git operations on Windows (preferring PowerShell over cmd for better path handling)\n"
        "- Build scripts that use PowerShell cmdlets or .NET APIs\n"
        "- Windows registry / WMI / CIM queries\n"
        "- Any task requiring Windows-specific APIs\n\n"
        "## When NOT to use\n"
        "- File operations: prefer FileRead / FileEdit / FileWrite\n"
        "- File searching: prefer Grep / Glob\n"
        "- Unix-style commands: prefer Bash tool (uses cmd.exe)\n\n"
        "## PowerShell edition guidance\n"
        "This tool uses Windows PowerShell 5.1 (powershell.exe) by default.\n"
        "- Pipeline chain operators `&&` and `||` are NOT available — they cause a parser error.\n"
        "  To run B only if A succeeds: `A; if ($?) { B }`. To chain unconditionally: `A; B`.\n"
        "- Ternary (`?:`), null-coalescing (`??`), and null-conditional (`?.`) operators are NOT "
        "available.\n"
        "  Use `if/else` and explicit `$null -eq` checks instead.\n"
        "- Avoid `2>&1` on native executables (wraps each line in ErrorRecord, sets `$?` to "
        "false).\n"
        "  stderr is already captured separately — don't redirect it.\n"
        "- Default file encoding is UTF-16 LE (with BOM). When writing files other tools will "
        "read,\n"
        "  pass `-Encoding utf8` to `Out-File` / `Set-Content`.\n"
        "- `ConvertFrom-Json` returns a PSCustomObject, not a hashtable. `-AsHashtable` is not "
        "available.\n\n"
        "## Guidelines\n"
        "- Prefer specialized tools (FileRead/FileEdit/FileWrite/Grep/Glob) when applicable\n"
        "- Use `description` to briefly explain what the command does (5-10 words)\n"
        "- Set `timeout` for long-running commands (max 600000ms)\n"
        "- Use `run_in_background=true` for long-running commands: they run without "
        "blocking you, and their output is automatically delivered back to you "
        "when they finish (use it for builds/tests/downloads you can wait on "
        "while doing other work)\n"
        "- Avoid interactive cmdlets: `Read-Host`, `Get-Credential`, `PromptForChoice` "
        "(the shell runs in -NonInteractive mode)\n"
        "- Commands run in the current working directory by default\n"
        "- Do NOT prefix commands with `cd` or `Set-Location` — use the `cwd` parameter instead\n"
        "- Environment variables: read with `$env:NAME`, set with `$env:NAME = \"value\"`\n"
        "- The sandbox restricts filesystem and network access by default; "
        "set `dangerously_disable_sandbox=true` only when the command truly "
        "needs unrestricted access\n\n"
        "## Output format\n"
        "- stdout is wrapped in <stdout>...</stdout> tags\n"
        "- stderr is wrapped in <stderr>...</stderr> tags\n"
        "- Non-zero exit codes are reported in <error>...</error> tags\n"
        "- Output exceeding 8000 characters is truncated\n"};
    return p;
}

nlohmann::json PowerShellTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {{"command", {{"type", "string"}, {"description", "The PowerShell command to execute"}}},
          {"description",
           {{"type", "string"},
            {"description", "Brief description of what the command does (5-10 words)"}}},
          {"timeout",
           {{"type", "integer"},
            {"description", "Timeout in milliseconds (max 600000)"},
            {"default", 120000}}},
          {"cwd",
           {{"type", "string"},
            {"description",
             "Working directory for the command (defaults to ctx.cwd). Use absolute paths."}}},
          {"run_in_background",
           {{"type", "boolean"},
            {"description", "Run command in background, return immediately with task id"},
            {"default", false}}},
          {"dangerously_disable_sandbox",
           {{"type", "boolean"},
            {"description", "Disable sandbox restrictions for this command"},
            {"default", false}}}}},
        {"required", {"command"}},
        {"additionalProperties", false}};
}

// ============================================================
// 权限检查
// ============================================================

PermissionResult PowerShellTool::check_permissions(const nlohmann::json& input,
                                                   const ToolContext& ctx) const {
    // #36：Bypass 模式完全放行
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    // #36：Plan 模式禁止执行命令
    if (deny_execute_by_mode(ctx.permission_mode)) {
        return PermissionResult::err(Error::Code::PermissionDenied,
                                     "Command execution is not allowed in plan mode. "
                                     "Switch to default mode to run commands.");
    }
    // #85：严格档——白名单放行，未命中需确认（headless 无确认通道 → 拒绝），
    //      破坏性命令直接拒绝不询问。判定与审计都在 enforce_strict_command 内。
    if (is_strict_mode(ctx.permission_mode)) {
        if (input.contains("command") && input["command"].is_string()) {
            const auto r = enforce_strict_command(input["command"].get<std::string>(), ctx, name());
            if (!r.allowed) {
                return PermissionResult::err(Error::Code::PermissionDenied, r.reason);
            }
        }
        return PermissionResult::ok();
    }
    // #36：Default 模式危险命令需用户确认
    if (input.contains("command") && input["command"].is_string()) {
        const std::string command = input["command"].get<std::string>();
        if (is_dangerous_command(command)) {
            const std::string question = std::format(
                "The command contains destructive patterns and requires your approval:\n\n"
                "```\n{}\n```\n\nAllow running this command?",
                command);
            if (!ask_user_confirm(ctx, question)) {
                return PermissionResult::err(
                    Error::Code::PermissionDenied,
                    "Command execution denied by user: " + command.substr(0, 120));
            }
        }
    }
    return PermissionResult::ok();
}

// ============================================================
// call() — 入口分发
// ============================================================

ResultV2<ToolResult> PowerShellTool::call(const nlohmann::json& input,
                                          const ToolContext& ctx) const {
    // 解析参数
    if (!input.contains("command") || !input["command"].is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "PowerShellTool: 'command' is required");
    }
    const std::string command = input["command"].get<std::string>();
    if (command.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "PowerShellTool: 'command' must not be empty");
    }

    int timeout_ms = kDefaultTimeoutMs;
    if (input.contains("timeout") && input["timeout"].is_number()) {
        timeout_ms = input["timeout"].get<int>();
        if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
        if (timeout_ms > kMaxTimeoutMs) timeout_ms = kMaxTimeoutMs;
    }
    // #137：按剩余墙钟封顶（未注入预算时 ctx.remaining_wall_clock_ms < 0，行为不变）
    if (ctx.remaining_wall_clock_ms >= 0) {
        const int clamped = shell_common::clamp_timeout_by_wall_clock(
            timeout_ms, ctx.remaining_wall_clock_ms);
        if (clamped != timeout_ms) {
            LOG_WARN("[powershell_tool] timeout {}ms -> {}ms (remaining wall clock {}ms)",
                     timeout_ms, clamped, ctx.remaining_wall_clock_ms);
            timeout_ms = clamped;
        }
    }

    bool run_in_background = false;
    if (input.contains("run_in_background") && input["run_in_background"].is_boolean()) {
        run_in_background = input["run_in_background"].get<bool>();
    }

    bool disable_sandbox = false;
    if (input.contains("dangerously_disable_sandbox") &&
        input["dangerously_disable_sandbox"].is_boolean()) {
        disable_sandbox = input["dangerously_disable_sandbox"].get<bool>();
    }

    // #188：该开关由模型写在 tool input 里 —— 提示注入可借它静默关掉隔离，
    //       而「关掉隔离」本身不该是模型能单方面决定的事。Bypass 档本就是全权
    //       （无需再问），其余档一律要人工确认；headless 无确认通道，
    //       ask_user_confirm 直接返回 false，即 fail-closed 拒绝。
    if (disable_sandbox && !is_bypass_mode(ctx.permission_mode)) {
        const std::string question = std::format(
            "The model asked to run this command with the sandbox DISABLED:\n\n"
            "```\n{}\n```\n\nAllow running it without sandbox restrictions?",
            command);
        if (!ask_user_confirm(ctx, question)) {
            return ResultV2<ToolResult>::err(
                Error::Code::PermissionDenied,
                "Sandbox disable was not approved; refusing to run the command.");
        }
    }

    // #35：命令级风险检测（破坏性/SSRF/env 泄露）——执行前拦截 + 审计
    // 手动审核（Default）模式下由用户确认放行；放行后禁用沙箱授予完整访问
    const auto risk = detect_shell_risk(command);
    if (risk != ShellRisk::None) {
        const std::string reason = shell_risk_description(risk);
        auto guard_event = [&](EventType t) {
            audit::AuditLogger::instance().log_security(t, reason, ctx.session_id, name());
        };
        if ((risk & ShellRisk::Destructive) != ShellRisk::None)
            guard_event(EventType::SecurityDangerousCommand);
        if ((risk & ShellRisk::SSRF) != ShellRisk::None)
            guard_event(EventType::SecuritySSRFAttempt);
        if ((risk & ShellRisk::EnvLeak) != ShellRisk::None)
            guard_event(EventType::SecurityEnvVarLeak);
        if (!is_bypass_mode(ctx.permission_mode) && !is_plan_mode(ctx.permission_mode)) {
            const std::string question = std::format(
                "The command is blocked by the security guard ({}):\n\n```\n{}\n```\n\n"
                "Allow running this command?",
                reason, command);
            if (ask_user_confirm(ctx, question)) {
                disable_sandbox = true;
            } else {
                return ResultV2<ToolResult>::err(Error::Code::PermissionDenied,
                                                 "Command execution denied by user: " + reason);
            }
        } else {
            return ResultV2<ToolResult>::err(Error::Code::PermissionDenied,
                                             "Command blocked by security guard: " + reason);
        }
    }

    // cwd：优先用参数，否则用 ctx.cwd
    std::string cwd = ctx.cwd;
    if (input.contains("cwd") && input["cwd"].is_string() &&
        !input["cwd"].get<std::string>().empty()) {
        cwd = input["cwd"].get<std::string>();
    }

    // #84 方案 A：restrictive 档敏感目标前置拦截（Windows 无 FS/网络沙盒后端的兜底，
    // macOS/Linux 与 seatbelt/bwrap 构成纵深）。命中即审计 + 用户确认；确认后放行并禁用沙箱。
    if (!disable_sandbox) {
        const auto sb_config = process::sandbox::SandboxConfig::restrictive(cwd);
        SandboxTargetPolicy target_policy;
        target_policy.network_isolated = sb_config.network_isolated;
        target_policy.allow_domains = sb_config.allow_domains;
        target_policy.deny_write = sb_config.deny_write;
        const auto target_risk = detect_sandbox_target_risk(command, target_policy);
        if (target_risk != SandboxTargetRisk::None) {
            const std::string reason = sandbox_target_risk_description(target_risk);
            audit::AuditLogger::instance().log_security(EventType::SecuritySandboxTargetBlocked,
                                                        reason, ctx.session_id, name());
            if (!is_bypass_mode(ctx.permission_mode) && !is_plan_mode(ctx.permission_mode)) {
                const std::string question = std::format(
                    "The command targets protected resources ({}):\n\n```\n{}\n```\n\n"
                    "Allow running this command (sandbox will be disabled)?",
                    reason, command);
                if (ask_user_confirm(ctx, question)) {
                    disable_sandbox = true;
                } else {
                    return ResultV2<ToolResult>::err(Error::Code::PermissionDenied,
                                                     "Command execution denied by user: " +
                                                         reason);
                }
            } else {
                return ResultV2<ToolResult>::err(
                    Error::Code::PermissionDenied,
                    "Command blocked by sandbox target guard: " + reason);
            }
        }
    }

    // 取消检查
    if (ctx.is_cancelled()) {
        return ResultV2<ToolResult>::err(Error::Code::Cancelled,
                                         "PowerShellTool: cancelled before execution");
    }

    if (run_in_background) {
        return execute_background(command, cwd, timeout_ms, disable_sandbox, ctx);
    }
    return execute_sync(command, cwd, timeout_ms, disable_sandbox, ctx);
}

// ============================================================
// execute_sync — 同步执行路径
// ============================================================

ResultV2<ToolResult> PowerShellTool::execute_sync(const std::string& command,
                                                  const std::string& cwd, int timeout_ms,
                                                  bool disable_sandbox,
                                                  const ToolContext& ctx) const {
    ctx.report_progress(std::format("Executing (PowerShell): {}", command));

    // 构建沙盒配置
    process::sandbox::SandboxConfig sb_config =
        disable_sandbox ? process::sandbox::SandboxConfig::permissive()
                        : process::sandbox::SandboxConfig::restrictive(cwd);

    // 包装命令：powershell.exe -NoProfile -NonInteractive -Command <command>
    auto wrapped = process::sandbox::SandboxAdapter::wrap_command(
        kShell, {kNoProfileFlag, kNonInteractiveFlag, kCommandFlag, command}, sb_config);

    // #84：不再被 was_wrapped 门控 —— 平台无后端时会如实上报 degraded
    sandbox_visibility_.report(wrapped, disable_sandbox, name(), ctx);

    process::ExecOptions opts;
    opts.cwd = cwd;
    opts.args = wrapped.args;
    // #84 方案 B：把策略侧派生的进程级约束交给执行层（Windows 上落为 Job Object）
    opts.isolation = wrapped.isolation;
    // #188：命令文本由模型生成，父进程环境里的模型凭据不继承给子进程
    opts.scrub_secrets = true;
    if (timeout_ms > 0) {
        opts.timeout = std::chrono::milliseconds(timeout_ms);
    }
    if (ctx.cancel_flag != nullptr) {
        const std::atomic<bool>* flag = ctx.cancel_flag;
        opts.is_cancelled = [flag]() { return flag->load(std::memory_order_acquire); };
    }

    auto exec_result = process::exec(wrapped.cmd, opts);
    if (exec_result.is_err()) {
        const auto& err = exec_result.error();
        std::string msg = std::format("Failed to execute PowerShell command: {}", err.message);
        return ResultV2<ToolResult>::err(err.code, std::move(msg));
    }

    const auto& out = exec_result.value();
    // #84 方案 B：进程级隔离到底有没有施加上，只有 exec 返回后才知道
    sandbox_visibility_.report_isolation(out.isolation, name(), ctx);

    std::string formatted = format_result(out);
    formatted = truncate_output(std::move(formatted));
    // #36：输出脱敏，密钥内容替换为 [REDACTED:label]
    formatted = redact_secrets(formatted);

    if (out.is_success()) {
        ctx.report_progress("Command completed successfully");
    } else if (out.timed_out) {
        ctx.report_progress("Command timed out");
    } else if (out.cancelled) {
        ctx.report_progress("Command cancelled");
    } else {
        ctx.report_progress("Command exited with code " + std::to_string(out.exit_code));
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(formatted)));
}

// ============================================================
// execute_background — 后台执行路径
// ============================================================

ResultV2<ToolResult> PowerShellTool::execute_background(const std::string& command,
                                                        const std::string& cwd, int timeout_ms,
                                                        bool disable_sandbox,
                                                        const ToolContext& ctx) const {
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(
            Error::Code::ConfigInvalid,
            "PowerShellTool: run_in_background=true requires TaskManager "
            "(ctx.task_manager_ptr is null)");
    }

    process::sandbox::SandboxConfig sb_config =
        disable_sandbox ? process::sandbox::SandboxConfig::permissive()
                        : process::sandbox::SandboxConfig::restrictive(cwd);
    auto wrapped = process::sandbox::SandboxAdapter::wrap_command(
        kShell, {kNoProfileFlag, kNonInteractiveFlag, kCommandFlag, command}, sb_config);

    // #84：后台路径此前完全没有沙箱状态上报，降级同样静默
    sandbox_visibility_.report(wrapped, disable_sandbox, name(), ctx);

    const size_t task_id = s_task_id_counter.fetch_add(1, std::memory_order_relaxed);
    std::string task_name = "ps:" + std::to_string(task_id) + ":" + command.substr(0, 40);

    // #166：后台任务结果回传——启动前登记 pending，任务结束时把结果回传追踪器。
    // 必须在 launch 之前 add_pending（理由同 BashTool：避免瞬时任务导致 pending 残留）。
    auto tracker = ctx.background_tracker;
    if (tracker) {
        tracker->add_pending(task_name);
    }

    auto& tm = ctx.task_manager();
    auto task = tm.launch(
        task_name,
        [command, cwd, timeout_ms, wrapped, task_name, tracker](
            const std::atomic<bool>& should_cancel) {
            // 保证恰好回传一次（含启动失败/异常路径），否则追踪器 pending 永久挂起。
            bool reported = false;
            auto report = [&](std::string text, bool is_err) {
                if (tracker && !reported) {
                    reported = true;
                    tracker->mark_done(
                        BackgroundTaskResult{task_name, "PowerShell", std::move(text), is_err});
                }
            };
            try {
                process::ExecOptions opts;
                opts.cwd = cwd;
                opts.args = wrapped.args;
                // #84 方案 B：后台路径同样带上进程级约束（超时/取消时连带杀全树）
                opts.isolation = wrapped.isolation;
                // #188：后台路径同样不把父进程凭据交给子进程
                opts.scrub_secrets = true;
                if (timeout_ms > 0) {
                    opts.timeout = std::chrono::milliseconds(timeout_ms);
                }
                opts.is_cancelled = [&should_cancel]() {
                    return should_cancel.load(std::memory_order_acquire);
                };

                auto exec_result = process::exec(wrapped.cmd, opts);
                if (exec_result.is_err()) {
                    const auto& err = exec_result.error();
                    report(std::format("Failed to execute PowerShell command: {}", err.message),
                           true);
                    throw std::runtime_error("Failed to execute PowerShell command: " + err.message);
                }
                auto& out = exec_result.value();
                if (out.isolation == process::IsolationOutcome::Failed) {
                    // #84 方案 B：后台路径拿不到 ctx（本 lambda 可能比调用方存活更久），
                    // 因此把降级事实写进被持久化的输出本身，避免静默丢失。
                    out.stderr_text =
                        "[workx] process isolation unavailable: job object not applied\n" +
                        out.stderr_text;
                }
                PSOutputRegistry::instance().store(task_name, out);
                // #166：格式化输出（截断 + 脱敏，与同步路径一致）回传追踪器
                std::string formatted = format_result(out);
                formatted = truncate_output(std::move(formatted), kMaxOutputChars);
                formatted = redact_secrets(formatted);
                report(std::move(formatted), !out.is_success());
            } catch (...) {
                // 兜底：任何未预期异常也解除 pending，避免 ReActLoop 收尾时无限等待
                report("Background command failed to execute.", true);
                throw;
            }
        },
        TaskType::Background);

    if (!task) {
        if (tracker) {
            tracker->remove_pending(task_name);  // 启动失败：撤销登记，避免污染 pending
        }
        return ResultV2<ToolResult>::err(Error::Code::InternalError,
                                         "PowerShellTool: failed to launch background task");
    }

    // 立即返回 task 信息（非阻塞）。完成后结果由 ReActLoop 自动回传模型。
    std::string task_name_str = task->getName();
    std::string result = "PowerShell command started in background with ID: " + task_name_str +
                         "\nCommand: " + command +
                         "\nIt runs without blocking you; its output will be delivered back to "
                         "you automatically when it completes.";

    ctx.report_progress("Background task started: " + task_name_str);

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
