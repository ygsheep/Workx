/**
 * @file git_checkpoint.cpp
 * @brief Issue #81：会话级 git 检查点实现
 * @details 所有 git 调用只读（rev-parse / diff / ls-files / status），
 *          唯一写操作是 rollback_tracked 的 git checkout —— 且只作用于
 *          基线中已存在的文件，新增与未跟踪文件一律不删除。
 */

#include "agent/util/git_checkpoint.h"

#include <chrono>
#include <map>
#include <string>
#include <vector>

#include "core/process/subprocess.h"

namespace agent::util {

namespace {

/// @brief 执行 git 命令并返回去尾空白的 stdout；失败/非零退出返回空串
/// @details 限时 3s：diff/numstat 在大仓库上可能比 rev-parse 慢，但不应阻塞会话。
std::string git_stdout(const std::string& cwd, const std::vector<std::string>& args) {
    try {
        auto res = process::exec("git", process::ExecOptions{
                                            .cwd = cwd,
                                            .args = args,
                                            .timeout = std::chrono::milliseconds(3000),
                                        });
        if (res.is_ok() && res.value().exit_code == 0) {
            std::string out = res.value().stdout_text;
            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
                out.pop_back();
            }
            return out;
        }
    } catch (...) {
        // git 缺失或异常时不阻断会话：调用方按空结果处理
    }
    return {};
}

/// @brief 按换行切分为非空行
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nl = text.find('\n', start);
        const std::string line =
            (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') {
            out.push_back(line.substr(0, line.size() - 1));
        } else if (!line.empty()) {
            out.push_back(line);
        }
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

/// @brief 解析 tab 分隔的 "<f1>\t<f2>\t<rest>" 行（git numstat / name-status 格式）
/// @return 字段数 >= 3 时返回 true
bool parse_tab_line(const std::string& line, std::string& f1, std::string& f2, std::string& rest) {
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) return false;
    const size_t t2 = line.find('\t', t1 + 1);
    if (t2 == std::string::npos) return false;
    f1 = line.substr(0, t1);
    f2 = line.substr(t1 + 1, t2 - t1 - 1);
    rest = line.substr(t2 + 1);
    return true;
}

/// @brief 字符串转整数，失败返回 0（numstat 对二进制文件输出 "-"）
int to_int_or_zero(const std::string& s) {
    try {
        return std::stoi(s);
    } catch (...) {
        return 0;
    }
}

}  // namespace

GitCheckpoint& GitCheckpoint::instance() {
    static GitCheckpoint inst;
    return inst;
}

bool GitCheckpoint::capture(const std::string& cwd) {
    if (info_.valid) return true;  // 幂等：会话内只捕获一次
    if (git_stdout(cwd, {"rev-parse", "--is-inside-work-tree"}) != "true") return false;

    info_.repo_root = git_stdout(cwd, {"rev-parse", "--show-toplevel"});
    info_.base_commit = git_stdout(cwd, {"rev-parse", "--short", "HEAD"});
    if (info_.repo_root.empty() || info_.base_commit.empty()) return false;

    info_.clean_at_capture = git_stdout(cwd, {"status", "--porcelain"}).empty();
    info_.valid = true;
    cwd_ = cwd;
    return true;
}

void GitCheckpoint::reset() {
    info_ = GitCheckpointInfo{};
    cwd_.clear();
}

GitDiffSummary GitCheckpoint::diff_since_base() const {
    GitDiffSummary out;
    if (!info_.valid) return out;

    out.base_commit = info_.base_commit;
    out.clean_at_capture = info_.clean_at_capture;
    collect_tracked_changes(out);
    collect_untracked(out);
    for (const auto& f : out.files) {
        out.total_insertions += f.insertions;
        out.total_deletions += f.deletions;
    }
    return out;
}

void GitCheckpoint::collect_tracked_changes(GitDiffSummary& out) const {
    // 状态来自 --name-status，行数来自 --numstat：两者按路径合并
    std::map<std::string, std::string> status_by_path;
    const std::string name_status = git_stdout(cwd_, {"diff", "--name-status", info_.base_commit});
    for (const auto& line : split_lines(name_status)) {
        // name-status 为两列：<status>\t<path>（重命名 R100\told\tnew 视为边缘情况忽略）
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        status_by_path[line.substr(tab + 1)] = line.substr(0, tab);
    }

    const std::string numstat = git_stdout(cwd_, {"diff", "--numstat", info_.base_commit});
    for (const auto& line : split_lines(numstat)) {
        std::string ins;
        std::string del;
        std::string path;
        if (!parse_tab_line(line, ins, del, path)) continue;

        GitFileChange change;
        change.path = path;
        change.insertions = to_int_or_zero(ins);
        change.deletions = to_int_or_zero(del);
        const auto it = status_by_path.find(path);
        change.status = (it != status_by_path.end()) ? it->second : std::string("M");
        out.files.push_back(std::move(change));
    }
}

void GitCheckpoint::collect_untracked(GitDiffSummary& out) const {
    const std::string listed = git_stdout(cwd_, {"ls-files", "--others", "--exclude-standard"});
    for (const auto& line : split_lines(listed)) {
        GitFileChange change;
        change.path = line;
        change.status = "?";
        out.files.push_back(std::move(change));
    }
}

std::string GitCheckpoint::format_summary(const GitDiffSummary& summary) {
    if (summary.files.empty()) {
        return "无文件改动（相对基线 " + summary.base_commit + "）";
    }

    std::string out = "改动 " + std::to_string(summary.files.size()) + " 个文件（+" +
                      std::to_string(summary.total_insertions) + " / -" +
                      std::to_string(summary.total_deletions) + "），基线 commit " +
                      summary.base_commit + "\n";
    if (!summary.clean_at_capture) {
        out += "注意：会话开始时工作区已有未提交改动，以下清单可能包含非本次产生的改动。\n";
    }
    for (const auto& f : summary.files) {
        out += "  " + f.status + "  " + f.path + "  +" + std::to_string(f.insertions) + " / -" +
               std::to_string(f.deletions) + "\n";
    }
    return out;
}

int GitCheckpoint::rollback_tracked(const GitDiffSummary& summary,
                                    std::vector<std::string>& skipped) const {
    if (!info_.valid) return 0;

    int done = 0;
    for (const auto& f : summary.files) {
        // 新增/未跟踪文件在基线中不存在，checkout 会失败；且删除不可逆，一律跳过
        if (f.status == "?" || f.status == "A") {
            skipped.push_back(f.path);
            continue;
        }
        auto res = process::exec("git", process::ExecOptions{
                                            .cwd = cwd_,
                                            .args = {"checkout", info_.base_commit, "--", f.path},
                                            .timeout = std::chrono::milliseconds(3000),
                                        });
        if (res.is_ok() && res.value().exit_code == 0) {
            ++done;
        } else {
            skipped.push_back(f.path);
        }
    }
    return done;
}

}  // namespace agent::util
