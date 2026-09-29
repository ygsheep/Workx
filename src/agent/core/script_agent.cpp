#include "agent/core/script_agent.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <utility>

#include "agent/core/mode_agent_common.h"  // run_whitelisted
#include "agent/core/query_tracker.h"      // QueryTracker
#include "liblogger/logger.h"

namespace agent {

namespace {

// 截断输出为置顶 N 行的可读摘要，避免整段回显刷屏 / 注入 TUI
std::string brief(const std::string& text, size_t limit = 12) {
    std::string out = text;
    // 去掉首尾空白，再压行
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(out.begin(), out.end(), not_space);
    auto last = std::find_if(out.rbegin(), out.rend(), not_space).base();
    if (first >= last) {
        return "(no output)";
    }
    out = std::string(first, last);

    size_t lines = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\n' && ++lines >= limit) {
            out.resize(i);
            out += "\n… (truncated)";
            break;
        }
    }
    return out;
}

}  // namespace

ScriptAgent::ScriptAgent(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

ReActResult ScriptAgent::run(const AgentGoal& goal, const std::string& goal_spec,
                             std::vector<ChatMessage>& messages, IReActObserver* /*observer*/) {
    const auto started = std::chrono::steady_clock::now();
    const auto done_ms = [&]() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    };

    ReActResult result;
    if (goal.type != AgentGoal::Script || goal.command.empty()) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "script agent requires agent.goal `script:<command>`";
        LOG_WARN("[script_agent] missing command (goal_spec='{}')", goal_spec);
        return result;
    }

    bool rejected = false;
    const process::ExecOutput out = run_whitelisted(goal.command, m_deps.cwd, &rejected);

    std::string summary;
    if (rejected) {
        summary =
            "**脚本被安全白名单拒绝执行**（命令不在允许列表）。\n\n"
            "目标：`" +
            goal_spec + "`";
        result.was_error = true;
        result.was_interrupted = false;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "script command rejected (not in allowlist)";
    } else if (out.exit_code == 0) {
        summary = "✓ 脚本执行成功（exit 0）。\n\n```\n" + brief(out.stdout_text) + "\n```";
        result.goal_status = GoalStatus::Achieved;
    } else {
        summary = "✗ 脚本执行失败（exit " + std::to_string(out.exit_code) + "）。\n\n" +
                  "[stderr]\n```\n" +
                  brief(out.stderr_text.empty() ? out.stdout_text : out.stderr_text) + "\n```";
        result.goal_status = GoalStatus::Failed;
    }

    // 记录到 QueryTracker 调用链（复用 GoalGuarded 的 verdict 落点约定）
    if (m_deps.tracker) {
        m_deps.tracker->record_verdict(result.goal_status,
                                       rejected ? "rejected (not in allowlist)"
                                                : "script exit=" + std::to_string(out.exit_code));
    }

    result.final_answer = summary;
    // 回填为最后一条 assistant 消息，供 UI 展示与 /resume 持久化
    messages.push_back(ChatMessage::assistant(summary));
    result.total_duration_ms = done_ms();
    LOG_INFO("[script_agent] goal='{}' exit={} rejected={}", goal_spec, out.exit_code, rejected);
    return result;
}

}  // namespace agent