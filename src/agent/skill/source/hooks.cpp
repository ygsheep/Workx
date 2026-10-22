/**
 * @file hooks.cpp
 * @brief Skill PreActivate 钩子执行器实现
 * @details 逐条 exec 钩子命令，捕获输出（截断防失控），失败不中断。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/skill/inclaude/hooks.h"

#include <algorithm>
#include <chrono>

#include "core/process/subprocess.h"

namespace agent::skill {

namespace {
constexpr size_t MAX_HOOK_OUTPUT_BYTES = 64 * 1024;  ///< 单条钩子输出上限 64KB

/// @brief 输出截断：取前 max_chars，超长补省略号
std::string truncate_output(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars) return text;
    return text.substr(0, max_chars) + "\n...(truncated)";
}
}  // anonymous namespace

std::vector<std::string> run_preactivate_hooks(const std::vector<std::string>& hooks,
                                               const std::string& cwd, int timeout_ms) {
    std::vector<std::string> results;
    for (const auto& hook : hooks) {
        if (hook.empty()) continue;
        agent::process::ExecOptions opts;
        opts.cwd = cwd;
        opts.timeout = std::chrono::milliseconds(timeout_ms);
        opts.max_output_bytes = MAX_HOOK_OUTPUT_BYTES;
        // hooks 是 shell 命令（echo 等内建语法），经系统 shell 执行。
        // 注意：cmd 必须无空格（避免 escape_arg 引号包裹导致 CreateProcess 失败），
        // 参数走 opts.args（Windows 分支同样按 arg 拼接并转义）
#if defined(_WIN32)
        const std::string command_line = "cmd.exe";
        opts.args = {"/d", "/s", "/c", hook};
#else
        const std::string command_line = "sh";
        opts.args = {"-c", hook};
#endif
        auto res = agent::process::exec(command_line, opts);
        if (res.is_ok() && res.value().exit_code == 0) {
            const auto& out = res.value();
            std::string combined = out.stdout_text;
            if (!out.stderr_text.empty()) {
                if (!combined.empty()) combined += "\n";
                combined += out.stderr_text;
            }
            results.push_back("[ok] " + hook + "\n" + truncate_output(combined, 4096));
        } else if (res.is_ok()) {
            const auto& out = res.value();
            std::string combined = out.stdout_text;
            if (!out.stderr_text.empty()) {
                if (!combined.empty()) combined += "\n";
                combined += out.stderr_text;
            }
            results.push_back("[fail] " + hook + " (exit " + std::to_string(out.exit_code) + ")\n" +
                              truncate_output(combined, 4096));
        } else {
            results.push_back("[fail] " + hook + ": " + res.error().message);
        }
    }
    return results;
}

std::string format_hook_output(const std::vector<std::string>& lines) {
    if (lines.empty()) return {};
    std::string out;
    for (const auto& line : lines) {
        out += "  " + line + "\n";
    }
    return out;
}

}  // namespace agent::skill
