#include "agent/core/watch_agent.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <string>
#include <thread>
#include <utility>

#include "agent/core/mode_agent_common.h"  // parse_watch_spec / materialize_cmd / ...
#include "agent/core/query_tracker.h"
#include "liblogger/logger.h"

namespace agent {

namespace fs = std::filesystem;

WatchAgent::WatchAgent(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

ReActResult WatchAgent::run(const AgentGoal& goal, const std::string& goal_spec,
                            std::vector<ChatMessage>& messages, IReActObserver* /*observer*/) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    };

    ReActResult result;
    if (goal.type != AgentGoal::Watch) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message =
            "watch agent requires agent.goal `watch:cmd=<tmpl>&path=<dir>&polls=<n>`";
        LOG_WARN("[watch_agent] non-watch goal routed (type={})", static_cast<int>(goal.type));
        return result;
    }
    WatchSpec spec = parse_watch_spec(goal);
    if (spec.cmd_template.empty()) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "watch agent requires a command template `cmd=<tmpl>`";
        LOG_WARN("[watch_agent] missing cmd template (goal='{}')", goal_spec);
        return result;
    }

    // 解析监控目录（相对路径挂到会话 cwd 下）
    std::error_code ec;
    fs::path cwd_canon = fs::weakly_canonical(fs::path(m_deps.cwd), ec);
    if (ec || !fs::is_directory(cwd_canon, ec)) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "cannot resolve working directory";
        LOG_WARN("[watch_agent] bad cwd '{}' (goal='{}')", m_deps.cwd, goal_spec);
        return result;
    }
    fs::path root = cwd_canon / fs::path(spec.path);
    ec.clear();
    root = fs::weakly_canonical(root, ec);
    if (ec || !fs::is_directory(root, ec)) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "watch path not a directory: " + spec.path;
        LOG_WARN("[watch_agent] bad watch path '{}' (goal='{}')", spec.path, goal_spec);
        return result;
    }
    // 隔离校验：监控根必须落在会话 cwd 子树内（防 symlink / ../ 逃逸到敏感目录）
    const std::string cwd_s = cwd_canon.lexically_normal().string();
    const std::string root_s = root.lexically_normal().string();
    {
        bool inside = root_s == cwd_s;
        if (!inside) {
            const std::string prefix = cwd_s + std::string(1, fs::path::preferred_separator);
            inside = root_s.rfind(prefix, 0) == 0;
        }
        if (!inside) {
            result.was_error = true;
            result.goal_status = GoalStatus::Failed;
            result.error_message = "watch path escapes working directory";
            LOG_WARN("[watch_agent] watch path escapes cwd '{}' (goal='{}')", spec.path, goal_spec);
            return result;
        }
    }

    std::string glob_err;
    const std::vector<std::string> items = expand_glob_cwd(root_s, spec.glob, &glob_err);
    if (!glob_err.empty()) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = glob_err;
        LOG_WARN("[watch_agent] glob error '{}': {}", spec.glob, glob_err);
        return result;
    }

    std::string baseline;
    bool command_failed = false;
    std::string last_change;
    for (int poll = 1; poll <= spec.max_polls; ++poll) {
        const std::string sig = snapshot_signature(root_s, items);
        if (poll == 1) {
            baseline = sig;  // 首轮仅建立基线
        } else if (sig != baseline) {
            // 检测到变化：物化并触发命令，刷新基线
            last_change = sig;
            const std::string cmd = materialize_cmd(spec.cmd_template, "");
            bool rejected = false;
            const process::ExecOutput out = run_whitelisted(cmd, root_s, &rejected);
            if (rejected) {
                command_failed = true;
                LOG_WARN("[watch_agent] triggered command rejected");
            } else if (out.exit_code != 0) {
                command_failed = true;
                LOG_WARN("[watch_agent] triggered command exit={}", out.exit_code);
            }
            baseline = sig;
        }
        // 非最后一轮：等待下一间隔（有界，默认 0 不等待）
        if (poll < spec.max_polls && spec.interval_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(spec.interval_ms));
        }
    }

    const bool changed = !last_change.empty();
    std::string summary = std::format("监控 `{}` 完成 {} 轮轮询（匹配 {} 个文件）。", root_s,
                                      spec.max_polls, items.size());
    if (items.empty()) {
        summary += "\n（无匹配文件，仅建立基线）";
    } else if (changed) {
        summary += command_failed ? "\n⚠ 检测到内容变化，触发的命令执行失败。"
                                  : "\n✓ 检测到内容变化，触发命令执行成功。";
    } else {
        summary += "\n（监控期间无内容变化）";
    }

    result.goal_status = (changed && command_failed) ? GoalStatus::Failed : GoalStatus::Achieved;
    if (changed && command_failed) {
        result.was_error = false;  // 展示为失败结果而非崩溃
    }
    if (m_deps.tracker) {
        m_deps.tracker->record_verdict(
            result.goal_status, std::format("watch polls={} changed={} command_failed={} files={}",
                                            spec.max_polls, changed, command_failed, items.size()));
    }

    result.final_answer = summary;
    messages.push_back(ChatMessage::assistant(summary));
    result.total_duration_ms = elapsed_ms();
    LOG_INFO("[watch_agent] goal='{}' root='{}' polls={} changed={} cmd_failed={}", goal_spec,
             root_s, spec.max_polls, changed, command_failed);
    return result;
}

}  // namespace agent