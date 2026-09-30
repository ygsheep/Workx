/**
 * @file bash_tool.cpp
 * @brief BashTool 实现
 * @details Shell 执行工具的具体实现，支持同步/后台/沙盒/超时/进度回调
 * @version 1.1.1
 * @date 2026-07
 */

#include "agent/tool/BashTool/bash_tool.h"

#include "agent/tool/ShellTool/shell_detector.h"
#include "agent/tool/ShellTool/shell_tool_common.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/secret_scanner.h"
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

/// 后台任务输出注册表容量上限（防止内存膨胀）
constexpr size_t kMaxRegistryEntries = 50;
/// 后台任务名唯一 id 计数器（M-3：避免 task_name 重复）
std::atomic<size_t> s_task_id_counter{0};

/// @brief 后台任务输出注册表（M-1 修复）
/// @details 保存后台任务的 ExecOutput，供 LLM 通过未来 GetBashOutputTool 查询。
///          线程安全，容量上限 kMaxRegistryEntries，FIFO 淘汰最旧条目。
class BashOutputRegistry {
   public:
    static BashOutputRegistry& instance() {
        static BashOutputRegistry inst;
        return inst;
    }

    void store(const std::string& task_name, const process::ExecOutput& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs[task_name] = out;
        // 超容量时淘汰最旧条目（按插入顺序，std::map 按键排序不够精确，
        // 但 task_name 含递增 id，按键排序近似 FIFO）
        if (m_outputs.size() > kMaxRegistryEntries) {
            m_outputs.erase(m_outputs.begin());
        }
    }

    /// 查询并返回输出副本；不存在返回空 optional
    std::optional<process::ExecOutput> get(const std::string& task_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outputs.find(task_name);
        if (it == m_outputs.end()) return std::nullopt;
        return it->second;
    }

    /// 移除条目（#23 P3：后台任务被取消后清理，不提供无意义的取消输出查询）
    void remove(const std::string& task_name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs.erase(task_name);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs.clear();
    }

    /// 当前条目数（仅日志/测试用）
    size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_outputs.size();
    }

   private:
    BashOutputRegistry() = default;
    mutable std::mutex m_mutex;
    std::map<std::string, process::ExecOutput> m_outputs;
};

/// @brief 获取当前 shell（运行期检测，首次调用后缓存）
/// @details Windows 上优先 Git Bash（支持 ls/grep/cat 等 Unix 命令），
///          无 Git Bash 时降级 cmd.exe。非 Windows 用 /bin/sh。
const shell_detect::ShellInfo& shell() { return shell_detect::detect(); }

}  // namespace

// ============================================================
// 元信息
// ============================================================

const std::string& BashTool::name() const {
    static const std::string n{"Bash"};
    return n;
}

const std::string& BashTool::description() const {
    static const std::string d{
        "Executes a shell command and returns stdout, stderr, and exit code. "
        "Supports timeout, working directory, and optional background execution."};
    return d;
}

const std::string& BashTool::prompt() const {
    // 运行期根据检测到的 shell 类型生成 prompt，确保 prompt 与实际 shell 一致
    const auto& sh = shell();

    // Unix 风格 shell（/bin/sh 或 Git Bash）— 支持 ls/grep/cat 等
    static const std::string unix_prompt{
        "Executes a shell command on the user's machine.\n\n"
        "## Shell: bash/sh (POSIX)\n"
        "This tool uses a POSIX-compatible shell. Commands use Unix shell syntax:\n"
        "- ls, grep, find, cat, rm, cp, mv are available\n"
        "- Use forward slashes `/` in paths\n"
        "- Use `;` to chain commands unconditionally, `&&` to chain with success check\n"
        "- Environment variables: `$VAR` (read), `export VAR=value` (set)\n"
        "- Pipes `|` and redirections `>` `>>` `2>&1` are supported\n\n"
        "## When to use\n"
        "- Running tests, builds, or scripts\n"
        "- Inspecting files with system tools (grep, find, ls)\n"
        "- Git operations\n"
        "- Any task requiring shell access\n\n"
        "## Guidelines\n"
        "- Prefer specialized tools (FileRead/FileEdit/FileWrite/Grep/Glob) when applicable\n"
        "- Use `description` to briefly explain what the command does (5-10 words)\n"
        "- Set `timeout` for long-running commands (max 600000ms)\n"
        "- Use `run_in_background=true` for long-running commands whose output "
        "you don't immediately need\n"
        "- Avoid interactive commands that require user input\n"
        "- Commands run in the current working directory by default\n"
        "- The sandbox restricts filesystem and network access by default; "
        "set `dangerously_disable_sandbox=true` only when the command truly "
        "needs unrestricted access\n\n"
        "## Output format\n"
        "- stdout is wrapped in <stdout>...</stdout> tags\n"
        "- stderr is wrapped in <stderr>...</stderr> tags\n"
        "- Non-zero exit codes are reported in <error>...</error> tags\n"
        "- Output exceeding 8000 characters is truncated\n"};

    // cmd.exe 降级 — 仅 Windows 命令
    static const std::string cmd_prompt{
        "Executes a command in Windows cmd.exe.\n\n"
        "## Shell: cmd.exe (Windows, degraded — Git Bash not found)\n"
        "This tool uses cmd.exe because Git Bash was not detected. "
        "Commands must use Windows cmd syntax:\n"
        "- Use `dir` instead of `ls`\n"
        "- Use `findstr` instead of `grep`\n"
        "- Use `where` instead of `which`\n"
        "- Use `type` instead of `cat`\n"
        "- Use `del` instead of `rm`\n"
        "- Use `copy` instead of `cp`\n"
        "- Use `move` instead of `mv`\n"
        "- Use `&` to chain commands unconditionally, `&&` to chain with success check\n"
        "- Environment variables: `%VAR%` (read), `set VAR=value` (set)\n\n"
        "## Tip\n"
        "Install Git for Windows to enable Unix-style commands (ls/grep/cat) "
        "in this tool. Alternatively, use the **PowerShell** tool which supports "
        "aliases for ls/cat/cp/mv/rm.\n\n"
        "## Guidelines\n"
        "- Prefer specialized tools (FileRead/FileEdit/FileWrite/Grep/Glob) when applicable\n"
        "- Use `description` to briefly explain what the command does (5-10 words)\n"
        "- Set `timeout` for long-running commands (max 600000ms)\n"
        "- Use `run_in_background=true` for long-running commands whose output "
        "you don't immediately need\n"
        "- Avoid interactive commands that require user input\n"
        "- Commands run in the current working directory by default\n"
        "- The sandbox restricts filesystem and network access by default; "
        "set `dangerously_disable_sandbox=true` only when the command truly "
        "needs unrestricted access\n\n"
        "## Output format\n"
        "- stdout is wrapped in <stdout>...</stdout> tags\n"
        "- stderr is wrapped in <stderr>...</stderr> tags\n"
        "- Non-zero exit codes are reported in <error>...</error> tags\n"
        "- Output exceeding 8000 characters is truncated\n"};

    return sh.is_unix ? unix_prompt : cmd_prompt;
}

nlohmann::json BashTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"command", {{"type", "string"}, {"description", "The shell command to execute"}}},
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
// call() — 入口分发
// ============================================================

// ============================================================
// 权限检查
// ============================================================

PermissionResult BashTool::check_permissions(const nlohmann::json& input,
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
// 执行
// ============================================================

ResultV2<ToolResult> BashTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 解析参数
    if (!input.contains("command") || !input["command"].is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::MissingArgument,
                                         "BashTool: 'command' is required");
    }
    const std::string command = input["command"].get<std::string>();
    if (command.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "BashTool: 'command' must not be empty");
    }

    int timeout_ms = kDefaultTimeoutMs;
    if (input.contains("timeout") && input["timeout"].is_number()) {
        timeout_ms = input["timeout"].get<int>();
        if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
        if (timeout_ms > kMaxTimeoutMs) timeout_ms = kMaxTimeoutMs;
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

    // 取消检查
    if (ctx.is_cancelled()) {
        return ResultV2<ToolResult>::err(Error::Code::Cancelled,
                                         "BashTool: cancelled before execution");
    }

    if (run_in_background) {
        return execute_background(command, cwd, timeout_ms, disable_sandbox, ctx);
    }
    return execute_sync(command, cwd, timeout_ms, disable_sandbox, ctx);
}

// ============================================================
// execute_sync — 同步执行路径
// ============================================================

ResultV2<ToolResult> BashTool::execute_sync(const std::string& command, const std::string& cwd,
                                            int timeout_ms, bool disable_sandbox,
                                            const ToolContext& ctx) const {
    // 上报开始
    ctx.report_progress(std::format("Executing: {}", command));

    // 构建沙盒配置（若启用）
    process::sandbox::SandboxConfig sb_config =
        disable_sandbox ? process::sandbox::SandboxConfig::permissive()
                        : process::sandbox::SandboxConfig::restrictive(cwd);

    // 包装命令：通过 shell 执行，使管道/重定向/复合命令可用
    const auto& sh = shell();
    auto wrapped =
        process::sandbox::SandboxAdapter::wrap_command(sh.cmd, {sh.flag, command}, sb_config);

    // #84：沙箱状态可见化 —— 原实现在 `if (wrapped.was_wrapped)` 门控内上报，
    // 而降级路径（Windows 无后端）恰好 was_wrapped=false，导致"裸跑且零提示"。
    sandbox_visibility_.report(wrapped, disable_sandbox, name(), ctx);

    // 构建 ExecOptions
    process::ExecOptions opts;
    opts.cwd = cwd;
    opts.args = wrapped.args;
    if (timeout_ms > 0) {
        opts.timeout = std::chrono::milliseconds(timeout_ms);
    }
    // 取消回调：绑定到 ctx
    if (ctx.cancel_flag != nullptr) {
        const std::atomic<bool>* flag = ctx.cancel_flag;
        opts.is_cancelled = [flag]() { return flag->load(std::memory_order_acquire); };
    }

    // 执行
    auto exec_result = process::exec(wrapped.cmd, opts);
    if (exec_result.is_err()) {
        const auto& err = exec_result.error();
        // 启动失败（命令不存在等）
        std::string msg = std::format("Failed to execute command: {}", err.message);
        return ResultV2<ToolResult>::err(err.code, std::move(msg));
    }

    const auto& out = exec_result.value();
    std::string formatted = format_result(out);
    formatted = truncate_output(std::move(formatted), kMaxOutputChars);
    // #36：输出脱敏，密钥内容替换为 [REDACTED:label]
    formatted = redact_secrets(formatted);

    // 上报完成
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

ResultV2<ToolResult> BashTool::execute_background(const std::string& command,
                                                  const std::string& cwd, int timeout_ms,
                                                  bool disable_sandbox,
                                                  const ToolContext& ctx) const {
    // 检查 TaskManager 是否可用
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::ConfigInvalid,
                                         "BashTool: run_in_background=true requires TaskManager "
                                         "(ctx.task_manager_ptr is null)");
    }

    // 沙盒配置
    process::sandbox::SandboxConfig sb_config =
        disable_sandbox ? process::sandbox::SandboxConfig::permissive()
                        : process::sandbox::SandboxConfig::restrictive(cwd);
    auto wrapped = process::sandbox::SandboxAdapter::wrap_command(
        shell().cmd, {shell().flag, command}, sb_config);

    // #84：后台路径此前完全没有沙箱状态上报，降级同样静默
    sandbox_visibility_.report(wrapped, disable_sandbox, name(), ctx);

    // M-3 修复：task_name 拼接唯一递增 id，避免不同任务重名
    // 格式：bash:<id>:<command 前 40 字符>
    const size_t task_id = s_task_id_counter.fetch_add(1, std::memory_order_relaxed);
    std::string task_name = "bash:" + std::to_string(task_id) + ":" + command.substr(0, 40);

    auto& tm = ctx.task_manager();
    auto task = tm.launch(
        task_name,
        [command, cwd, timeout_ms, wrapped, task_name](const std::atomic<bool>& should_cancel) {
            process::ExecOptions opts;
            opts.cwd = cwd;
            opts.args = wrapped.args;
            if (timeout_ms > 0) {
                opts.timeout = std::chrono::milliseconds(timeout_ms);
            }
            opts.is_cancelled = [&should_cancel]() {
                return should_cancel.load(std::memory_order_acquire);
            };

            auto exec_result = process::exec(wrapped.cmd, opts);
            // M-1 修复：
            // 1. exec 启动失败（is_err）抛异常 → TaskManager 标记 Failed（发布 TaskFailedEvent）
            //    原实现 (void)exec_result 会导致 TaskManager 错误地标记 Completed
            if (exec_result.is_err()) {
                const auto& err = exec_result.error();
                throw std::runtime_error("Failed to execute command: " + err.message);
            }
            // 2. exec 成功（即使非零退出/超时/取消）→ 保存输出到注册表，供 LLM 查询
            //    非零退出/超时不视为 task 失败（命令执行了，只是结果非成功），符合 cc 行为
            //    任务被取消（#23 P3）：进程组已被销毁，输出无意义 → 清理注册表
            auto& out = exec_result.value();
            if (out.cancelled) {
                BashOutputRegistry::instance().remove(task_name);
            } else {
                BashOutputRegistry::instance().store(task_name, out);
            }
        },
        TaskType::Background);

    if (!task) {
        return ResultV2<ToolResult>::err(Error::Code::InternalError,
                                         "BashTool: failed to launch background task");
    }

    // 立即返回 task 信息
    std::string task_name_str = task->getName();
    std::string result = "Command running in background with ID: " + task_name_str +
                         "\nCommand: " + command +
                         "\nOutput will be available when the task completes.";

    ctx.report_progress("Background task started: " + task_name_str);

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
