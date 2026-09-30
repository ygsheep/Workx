/**
 * @file git_checkpoint.h
 * @brief Issue #81：会话级 git 检查点 —— 记录基线 commit，回答"agent 改了什么"
 * @details 定位是**只读安全网 + 人工回滚入口**，不是自动快照：
 *          - 不改变工作区状态（不 stash、不 commit、不建 worktree）
 *          - 不注册为工具，模型无法自主回滚（避免误回滚掉用户自己的改动）
 *          - 只回答"相对会话开始时改了哪些文件、增删多少行"，供 review 与人工回滚
 *
 *          会话级单例：在 run 开始时捕获一次基线，之后跨 turn 复用。
 *          非 git 仓库时 valid=false，所有查询返回空结果（不阻断 Agent）。
 */

#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace agent::util {

/// @brief 展示用的短 sha 长度（与 git 默认 abbrev 一致）
constexpr size_t kShortShaLen = 7;

/// @brief git 检查点信息（会话开始时捕获的基线）
struct GitCheckpointInfo {
    bool valid = false;       ///< 是否在 git 仓库内且捕获成功
    std::string repo_root;    ///< 仓库根目录（绝对路径）
    std::string base_commit;  ///< 基线 commit（完整 sha，避免大仓库短 sha 歧义）
    bool clean_at_capture = false;  ///< 捕获时工作区是否干净
};

/// @brief 单个文件相对基线的改动
struct GitFileChange {
    std::string path;    ///< 相对仓库根的路径
    std::string status;  ///< M 修改 / A 新增（已暂存）/ D 删除 / ? 未跟踪
    int insertions = 0;  ///< 新增行数
    int deletions = 0;   ///< 删除行数
};

/// @brief 相对基线的改动汇总
struct GitDiffSummary {
    std::vector<GitFileChange> files;
    int total_insertions = 0;
    int total_deletions = 0;
    std::string base_commit;
    /// @brief 捕获时工作区是否干净
    /// @details false 表示清单里可能混有用户自己的未提交改动，回滚需谨慎。
    bool clean_at_capture = false;
};

/// @brief #81：会话级 git 检查点（单例）
class GitCheckpoint {
   public:
    /// @brief 获取单例
    static GitCheckpoint& instance();

    /// @brief 捕获基线 commit（按目录幂等）
    /// @param cwd 工作目录（用于定位仓库根）
    /// @details 已捕获且目录未变时直接复用；目录变化（TUI 切换会话/cwd）则丢弃旧基线重新捕获，
    ///          避免用 A 仓库的 base commit 去 diff/回滚 B 仓库。
    /// @return 是否处于 git 仓库并成功捕获
    bool capture(const std::string& cwd);

    /// @brief 重置（会话切换 / 测试隔离用）
    void reset();

    /// @brief 当前检查点信息
    const GitCheckpointInfo& info() const { return info_; }

    /// @brief 统计相对基线的改动（已跟踪改动 + 未跟踪新文件）
    GitDiffSummary diff_since_base() const;

    /// @brief 将改动汇总格式化为可读文本（供 /diff 与 headless 收尾报告）
    /// @details 展示用短 sha（内部仍以完整 sha 参与 git 命令，避免歧义）。
    static std::string format_summary(const GitDiffSummary& summary);

    /// @brief 取展示用短 sha（完整 sha 直接返回前 kShortShaLen 位，已是短 sha 则原样返回）
    static std::string short_sha(const std::string& full_sha);

    /// @brief 回滚已跟踪文件的改动到基线
    /// @details 只处理 M / D（基线中已存在的文件）；新增与未跟踪文件**不删除**
    ///          ——删除是不可逆的，且无法区分是 agent 建的还是用户建的，交由人工处理。
    /// @param summary 待回滚的改动清单（通常来自 diff_since_base）
    /// @param[out] skipped 未回滚的文件路径（新增/未跟踪/回滚失败）
    /// @return 成功回滚的文件数
    int rollback_tracked(const GitDiffSummary& summary, std::vector<std::string>& skipped) const;

   private:
    GitCheckpoint() = default;

    /// @brief 收集已跟踪文件的改动（numstat 行数 + name-status 状态）
    void collect_tracked_changes(GitDiffSummary& out) const;

    /// @brief 收集未跟踪的新文件（仅列路径，不统计行数）
    void collect_untracked(GitDiffSummary& out) const;

    /// @brief 归一化 numstat 路径：重命名输出形如 "old => new" / "dir/{old => new}"
    static std::string normalize_numstat_path(const std::string& raw);

    std::string cwd_;  ///< 捕获时的工作目录（仅供 const 查询读取，capture 内写入）
    GitCheckpointInfo info_;
    mutable std::mutex
        mtx_;  ///< 保护 cwd_/info_：capture、reset 与查询可能跨线程并发（TUI 命令线程）
};

}  // namespace agent::util
