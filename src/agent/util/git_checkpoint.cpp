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

/// @brief 取 name-status 行的状态列与路径列
/// @details 普通行为 "<status>\t<path>"；重命名行为 "R100\t<old>\t<new>"，
///          此时路径取**新路径**（回滚与展示都以当前文件名为准）。
bool parse_name_status(const std::string& line, std::string& status, std::string& path) {
    const size_t t1 = line.find('\t');
    if (t1 == std::string::npos) return false;
    status = line.substr(0, t1);
    const size_t t2 = line.find('\t', t1 + 1);
    if (status[0] == 'R' || status[0] == 'C') {
        if (t2 == std::string::npos) return false;
        path = line.substr(t2 + 1);
        status = std::string(1, status[0]);  // R100 → R，展示统一
        return true;
    }
    path = line.substr(t1 + 1);
    return !path.empty();
}

}  // namespace

GitCheckpoint& GitCheckpoint::instance() {
    static GitCheckpoint inst;
    return inst;
}

bool GitCheckpoint::capture(const std::string& cwd) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (info_.valid) {
        if (cwd_ == cwd) return true;  // 同目录：会话内只捕获一次
        // 换了目录：旧基线属于别的仓库，必须重捕获（否则 diff/回滚会作用于错误仓库）。
        // 这里内联清空而非调用 reset()：mtx_ 非递归，二次加锁会死锁。
        info_ = GitCheckpointInfo{};
        cwd_.clear();
    }
    // 一次 rev-parse 取回三项（是否仓库 / 仓库根 / 基线 commit）：
    // Windows 下每个 git 子进程约 300ms+，run 开头同步调用必须尽量少（原 4 次 → 2 次）。
    const std::string meta =
        git_stdout(cwd, {"rev-parse", "--is-inside-work-tree", "--show-toplevel", "HEAD"});
    const std::vector<std::string> meta_lines = split_lines(meta);
    if (meta_lines.size() < 3 || meta_lines[0] != "true") return false;

    info_.repo_root = meta_lines[1];
    // 存完整 sha：短 sha 在提交量增长后会歧义，git checkout 可能被拒绝
    info_.base_commit = meta_lines[2];
    if (info_.repo_root.empty() || info_.base_commit.empty()) return false;

    info_.clean_at_capture = git_stdout(cwd, {"status", "--porcelain"}).empty();
    info_.valid = true;
    cwd_ = cwd;
    return true;
}

void GitCheckpoint::reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    info_ = GitCheckpointInfo{};
    cwd_.clear();
}

GitDiffSummary GitCheckpoint::diff_since_base() const {
    std::lock_guard<std::mutex> lock(mtx_);
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
        std::string status;
        std::string path;
        if (!parse_name_status(line, status, path)) continue;
        status_by_path[path] = status;
    }

    const std::string numstat = git_stdout(cwd_, {"diff", "--numstat", info_.base_commit});
    for (const auto& line : split_lines(numstat)) {
        std::string ins;
        std::string del;
        std::string path;
        if (!parse_tab_line(line, ins, del, path)) continue;
        path = normalize_numstat_path(path);  // 重命名行统一取新路径，与 name-status 对齐

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

std::string GitCheckpoint::short_sha(const std::string& full_sha) {
    if (full_sha.size() <= kShortShaLen) return full_sha;
    return full_sha.substr(0, kShortShaLen);
}

std::string GitCheckpoint::normalize_numstat_path(const std::string& raw) {
    const std::string arrow = " => ";
    const size_t pos = raw.find(arrow);
    if (pos == std::string::npos) return raw;  // 普通行

    std::string prefix = raw.substr(0, pos);
    std::string rest = raw.substr(pos + arrow.size());
    // "dir/{old => new}" 形式：前缀保留到 '{' 之前，拼上新文件名
    const size_t brace = prefix.find('{');
    if (brace != std::string::npos) {
        rest = prefix.substr(0, brace) + rest;
    }
    if (!rest.empty() && rest.back() == '}') rest.pop_back();  // 去掉闭合花括号
    return rest;
}

std::string GitCheckpoint::format_summary(const GitDiffSummary& summary) {
    const std::string shown_base = short_sha(summary.base_commit);
    if (summary.files.empty()) {
        return "无文件改动（相对基线 " + shown_base + "）";
    }

    std::string out = "改动 " + std::to_string(summary.files.size()) + " 个文件（+" +
                      std::to_string(summary.total_insertions) + " / -" +
                      std::to_string(summary.total_deletions) + "），基线 commit " + shown_base +
                      "\n";
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
    std::lock_guard<std::mutex> lock(mtx_);
    if (!info_.valid) return 0;

    int done = 0;
    for (const auto& f : summary.files) {
        // 新增/未跟踪文件在基线中不存在，checkout 会失败；且删除不可逆，一律跳过。
        // 重命名（R）的新路径同样不在基线中 → checkout 失败后计入 skipped，由人工处理。
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
