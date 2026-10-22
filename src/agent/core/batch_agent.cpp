#include "agent/core/batch_agent.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "agent/core/mode_agent_common.h"  // parse_batch_spec / materialize_cmd / ...
#include "agent/core/query_tracker.h"
#include "liblogger/logger.h"

namespace agent {

namespace {

struct BatchItemResult {
    std::string item;
    bool skipped = false;   // item 无法安全引用
    bool rejected = false;  // 物化命令未过白名单
    bool timedout = false;  // 整体未在截止前完成
    int exit_code = -1;
    std::string out_brief;  // stdout/stderr 摘要
};

// BatchAgent 单次 run 的整体截止（毫秒）。run_whitelisted 单条 exec 已有 60s
// 超时，但 n 条 * 并发下可能长时间阻塞；超过截止则未开始/未完成的条目标记
// timedout，避免 join 无限挂起。默认 5 分钟。
constexpr std::chrono::milliseconds kBatchDeadline(300000);

}  // namespace

BatchAgent::BatchAgent(GoalAgentDeps deps) : m_deps(std::move(deps)) {}

ReActResult BatchAgent::run(const AgentGoal& goal, const std::string& goal_spec,
                            std::vector<ChatMessage>& messages, IReActObserver* /*observer*/) {
    const auto started = std::chrono::steady_clock::now();

    ReActResult result;
    BatchSpec spec = parse_batch_spec(goal);
    if (goal.type != AgentGoal::Batch) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "batch agent requires agent.goal `batch:cmd=<tmpl>&glob=<pattern>`";
        LOG_WARN("[batch_agent] non-batch goal routed (type={})", static_cast<int>(goal.type));
        return result;
    }
    if (spec.cmd_template.empty()) {
        result.was_error = true;
        result.goal_status = GoalStatus::Failed;
        result.error_message = "batch agent requires a command template `cmd=<tmpl>`";
        LOG_WARN("[batch_agent] missing cmd template (goal='{}')", goal_spec);
        return result;
    }

    std::string glob_err;
    const std::vector<std::string> items = expand_glob_cwd(m_deps.cwd, spec.glob, &glob_err);
    if (items.empty()) {
        std::string summary;
        if (!glob_err.empty()) {
            result.was_error = true;
            result.goal_status = GoalStatus::Failed;
            result.error_message = glob_err;
            summary = "✗ 批处理无法展开: " + glob_err;
        } else {
            result.was_error = false;
            result.goal_status = GoalStatus::Achieved;  // 无可执行项，视为空成功
            summary = "✓ 批处理完成（无匹配项，glob=`" + spec.glob + "`）";
        }
        result.final_answer = summary;
        messages.push_back(ChatMessage::assistant(summary));
        result.total_duration_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        LOG_INFO("[batch_agent] goal='{}' glob='{}' -> {} items ({})", goal_spec, spec.glob,
                 items.size(), glob_err);
        return result;
    }

    // 用例并行执行；并发度上限 = 条目数
    const size_t n = items.size();
    const size_t workers = std::min(spec.concurrency, n);
    std::vector<BatchItemResult> results(n);
    for (size_t i = 0; i < n; ++i) {
        results[i].item = items[i];
    }

    std::atomic<size_t> next{0};
    std::atomic<bool> expired{false};
    const auto deadline = std::chrono::steady_clock::now() + kBatchDeadline;
    const auto worker = [&]() {
        for (;;) {
            const size_t idx = next.fetch_add(1);
            if (idx >= n) break;
            if (expired.load(std::memory_order_relaxed)) {
                results[idx].timedout = true;  // 整体截止已到，不再启动新的 exec
                continue;
            }
            auto& r = results[idx];
            const std::string cmd = materialize_cmd(spec.cmd_template, items[idx]);
            if (cmd.empty()) {
                r.skipped = true;  // item 含 shell 敏感字符，拒绝注入
                continue;
            }
            // 启动前复查截止：已过期则本轮条目直接标记 timedout，不再分流副作用
            if (std::chrono::steady_clock::now() >= deadline) {
                expired.store(true, std::memory_order_relaxed);
                results[idx].timedout = true;
                continue;
            }
            bool rejected = false;
            const process::ExecOutput out = run_whitelisted(cmd, m_deps.cwd, &rejected);
            r.rejected = rejected;
            r.exit_code = out.exit_code;
            const std::string txt = out.stderr_text.empty() ? out.stdout_text : out.stderr_text;
            r.out_brief = txt.substr(0, 160);
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workers);
    for (size_t i = 0; i < workers; ++i) {
        threads.emplace_back(worker);
    }
    for (auto& t : threads) {
        t.join();
    }

    // 汇总
    int ok = 0, failed = 0, skipped = 0, rejected = 0, timedout = 0;
    std::string summary;
    summary.reserve(items.size() * 80);
    summary = "批量执行 " + std::to_string(n) + " 项（glob=`" + spec.glob + "`）：\n\n";
    for (const auto& r : results) {
        if (r.timedout) {
            ++timedout;
            summary += "  ⏱ `" + r.item + "` 整体截止超时，未执行\n";
        } else if (r.rejected) {
            ++rejected;
            summary += "  ⚠ `" + r.item + "` 命令被白名单拒绝\n";
        } else if (r.skipped) {
            ++skipped;
            summary += "  ⚠ `" + r.item + "` 路径含不安全字符，已跳过\n";
        } else if (r.exit_code == 0) {
            ++ok;
            summary += "  ✓ `" + r.item + "` (exit 0)\n";
        } else {
            ++failed;
            summary += "  ✗ `" + r.item + "` (exit " + std::to_string(r.exit_code) + ")";
            if (!r.out_brief.empty()) {
                summary += " — " + r.out_brief;
            }
            summary += "\n";
        }
    }
    summary += "\n完成：✓" + std::to_string(ok) + " ✗" + std::to_string(failed) + " 跳过" +
               std::to_string(skipped) + " 拒绝" + std::to_string(rejected) + " 超时" +
               std::to_string(timedout);

    result.goal_status = (failed == 0 && rejected == 0 && skipped == 0 && timedout == 0)
                             ? GoalStatus::Achieved
                             : GoalStatus::Failed;
    if (m_deps.tracker) {
        m_deps.tracker->record_verdict(
            result.goal_status,
            std::format("batch ok={} failed={} skipped={} rejected={} timedout={}", ok, failed,
                        skipped, rejected, timedout));
    }

    result.final_answer = summary;
    messages.push_back(ChatMessage::assistant(summary));
    result.total_duration_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    LOG_INFO("[batch_agent] goal='{}' items={} ok={} failed={} skipped={} rejected={}", goal_spec,
             n, ok, failed, skipped, rejected);
    return result;
}

}  // namespace agent