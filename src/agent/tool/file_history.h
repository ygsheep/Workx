/**
 * @file file_history.h
 * @brief 文件历史管理器（会话内多版本备份）
 * @details 用于 FileEditTool 写入前保存版本快照，支持 undo/多步回滚。
 *          版本存储在内存中，会话结束即清理（与 Claude Code fileHistory 行为一致）。
 *          - 每个 file_path 维护一个版本列表（按时间倒序，最新在前）
 *          - 默认上限 20 个版本/文件，超出自动裁剪最旧版本
 *          - 线程安全（mutex 保护）
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent::tool {

/// @brief 文件版本记录
struct FileVersion {
    std::string version_id;                           ///< 版本 ID（时间戳 + 序号）
    std::chrono::system_clock::time_point timestamp;  ///< 保存时间
    std::string content;                              ///< UTF-8 内容（LF 规范化）
    std::string operation;                            ///< 操作描述
};

/// @brief 文件历史管理器（单例，会话内多版本备份）
class FileHistory {
   public:
    /// @brief 获取单例
    static FileHistory& instance();

    /// @brief 保存文件当前版本（在编辑前调用）
    /// @param file_path 文件路径（建议使用 POSIX 规范化路径作为 key）
    /// @param content 当前 UTF-8 内容
    /// @param operation 操作描述（如 "before_edit", "before_create"）
    /// @return 版本 ID（可用于后续查询/回滚）
    std::string save_version(const std::string& file_path, const std::string& content,
                             const std::string& operation = "before_edit");

    /// @brief 获取文件的所有版本（按时间倒序，最新在前）
    std::vector<FileVersion> get_versions(const std::string& file_path) const;

    /// @brief 获取特定版本
    std::optional<FileVersion> get_version(const std::string& file_path,
                                           const std::string& version_id) const;

    /// @brief 获取最新版本（用于 undo）
    std::optional<FileVersion> get_latest_version(const std::string& file_path) const;

    /// @brief 清除文件的所有版本
    void clear_versions(const std::string& file_path);

    /// @brief 设置每个文件的最大版本数（默认 20）
    void set_max_versions(size_t max) { max_versions_ = max; }

    /// @brief 获取当前最大版本数
    size_t get_max_versions() const { return max_versions_; }

    /// @brief 清除所有历史（用于测试）
    void clear_for_test();

   private:
    FileHistory();
    FileHistory(const FileHistory&) = delete;
    FileHistory& operator=(const FileHistory&) = delete;

    /// @brief 裁剪旧版本（超过 max_versions_ 时移除最旧版本）
    void prune_old_versions_locked(const std::string& file_path);

    mutable std::mutex mutex_;
    size_t max_versions_ = 20;
    std::map<std::string, std::vector<FileVersion>> history_;
    size_t next_id_ = 0;
};

}  // namespace agent::tool
