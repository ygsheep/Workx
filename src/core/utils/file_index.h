/**
 * @file file_index.h
 * @brief FileIndex — 文件索引与搜索
 * @details TUI 启动时扫描工作目录构建文件索引，支持按文件名搜索和补全
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

namespace agent {

/// @brief FileIndex — 文件索引与搜索
///
/// TUI 启动时扫描工作目录（BFS）构建文件索引：
/// - 支持按文件名子串搜索（大小写不敏感）
/// - 空查询返回最近修改的 15 个文件
/// - 非空查询返回名称包含查询串的文件（最多 50 个）
/// - 自动跳过 .git/build/node_modules 等目录
class FileIndex {
   public:
    /// @brief 确保后台构建线程 join，避免 std::thread 析构 terminate
    ~FileIndex() {
        if (m_build_thread.joinable()) {
            m_build_thread.join();
        }
    }

    /// @brief 索引条目
    struct Entry {
        std::string name;                          ///< 文件名（如 "main.cpp"）
        std::string relative_path;                 ///< 相对路径（如 "src/app/main.cpp"）
        std::filesystem::file_time_type modified;  ///< 修改时间
        bool is_directory{false};                  ///< 是否为目录
    };

    /// @brief 默认空查询返回的文件数
    static constexpr size_t DEFAULT_EMPTY_LIMIT = 15;

    /// @brief 默认搜索查询返回的文件数
    static constexpr size_t DEFAULT_SEARCH_LIMIT = 50;

    /// @brief 构建文件索引（BFS 遍历）
    /// @param cwd 工作目录
    /// @param max_files 最大索引文件数（默认 10000）
    void build(const std::string& cwd, size_t max_files = 10000);

    /// @brief 异步构建文件索引（后台线程执行，不阻塞调用方）
    /// @param cwd 工作目录
    /// @param max_files 最大索引文件数
    /// @details 已在构建或已就绪时直接返回。TUI 启动时用此接口替代同步 build，
    ///          避免扫描大目录阻塞界面出现；@ 补全面板在未就绪时返回空（见
    ///          search_files），可通过 wait_ready 等待后台完成。
    void build_async(const std::string& cwd, size_t max_files = 10000);

    /// @brief 等待索引构建完成
    /// @param timeout_ms 最大等待毫秒；<=0 表示无限等待
    /// @return 就绪返回 true，超时返回 false
    /// @details 轮询 m_ready_flag（10ms 间隔），构建通常远小于 1s，开销可忽略。
    ///          注意：仅保证"已有一次构建完成"，不感知 refresh 触发的后台重建是否
    ///          结束（重建期间 search 读到旧索引，完成后自动更新）。
    bool wait_ready(int timeout_ms = 0) const;

    /// @brief 优雅关闭后台构建线程（程序退出前调用）
    /// @details join 等待后台 build 结束，避免线程访问已析构单例。
    void shutdown();

    /// @brief 搜索文件
    /// @param query 搜索查询（空字符串 → 返回最近修改的文件）
    /// @param limit 最大返回数
    /// @return 匹配的文件条目列表（按修改时间倒序）
    std::vector<Entry> search(const std::string& query, size_t limit = DEFAULT_SEARCH_LIMIT) const;

    /// @brief 搜索文件名（仅返回路径字符串）
    /// @param query 搜索查询
    /// @param limit 最大返回数
    /// @return 匹配的文件路径列表
    std::vector<std::string> search_paths(const std::string& query,
                                          size_t limit = DEFAULT_SEARCH_LIMIT) const;

    /// @brief 索引是否已就绪
    /// @return 已构建返回 true
    bool is_ready() const;

    /// @brief 获取索引文件数
    /// @return 文件数量
    size_t size() const;

    /// @brief 标记索引为脏（下次 refresh_if_needed 时重建）
    /// @details 工具（FileWriteTool 等）写入/删除文件后调用。
    ///          无锁原子操作，可安全在工具线程频繁调用。
    void mark_dirty();

    /// @brief 按需刷新索引（脏或超过最小间隔时重建）
    /// @param min_interval_ms 最小刷新间隔（毫秒），用于防抖避免频繁重建
    /// @return 实际触发了重建返回 true
    /// @details 典型场景：TUI `@` 补全面板首次打开或查询变更时调用。
    ///          若索引从未构建过（ready_=false）则直接返回 false。
    bool refresh_if_needed(int64_t min_interval_ms);

   private:
    std::vector<Entry> entries_;
    bool ready_{false};
    mutable std::mutex mutex_;

    std::atomic<bool> m_dirty{true};  ///< 脏标记：工具写入后置 true，build 后清 false
    std::chrono::steady_clock::time_point m_last_build_ts;  ///< 上次构建时间（用于防抖）
    std::string m_cwd_;  ///< 上次构建的工作目录（refresh 复用）

    // ---- 异步构建状态（build_async / wait_ready / shutdown）----
    std::thread m_build_thread;                    ///< 后台构建线程（shutdown 时 join）
    std::atomic<bool> m_build_in_progress{false};  ///< 是否已有后台构建在进行/已请求
    std::atomic<bool> m_ready_flag{
        false};  ///< 索引就绪标志（is_ready/wait_ready 读取，不碰 mutex_ 避免阻塞）

    /// @brief 判断目录是否应跳过
    /// @param dir_name 目录名
    /// @return 应跳过返回 true
    static bool should_skip_dir(const std::string& dir_name);
};

/// @brief 全局 FileIndex 实例
/// @return FileIndex 单例引用
FileIndex& global_file_index();

}  // namespace agent
