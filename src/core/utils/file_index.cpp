/**
 * @file file_index.cpp
 * @brief FileIndex 实现
 * @details 文件索引构建（BFS）与搜索实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/utils/file_index.h"

#include <algorithm>
#include <cctype>
#include <queue>
#include <set>

namespace agent {

namespace fs = std::filesystem;

// ============================================================
// 私有辅助
// ============================================================

bool FileIndex::should_skip_dir(const std::string& dir_name) {
    // 常见需要跳过的目录（含隐藏目录中的无关项）
    static const std::set<std::string> skip_dirs = {
        ".git",     ///< Git 元数据（大量无关文件）
        ".vs",      ///< Visual Studio 临时文件
        ".vscode",  ///< VS Code 配置（可选，但通常无需搜索）
        ".idea",    ///< JetBrains 配置
        ".cache",   ///< 缓存目录
        "build",
        "out",
        "bin",
        "obj",
        "node_modules",
        "__pycache__",
        ".venv",
        "venv",
        "vcpkg_installed",
        "CMakeFiles",
        "cmake-build-debug",
        "cmake-build-release",
        "dist",
        "target",
        "Debug",
        "Release",
        "x64",
        "Win32",
        "ARM",
        "ARM64",
    };

    return skip_dirs.count(dir_name) > 0;
}

/// @brief 小写转换
static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

// ============================================================
// 构建索引
// ============================================================

void FileIndex::build(const std::string& cwd, size_t max_files) {
    fs::path root(cwd);
    std::error_code ec;

    if (!fs::is_directory(root, ec)) {
        // 无效目录也要标记"完成"，避免 wait_ready 无限等待
        std::lock_guard<std::mutex> lock(mutex_);
        ready_ = false;
        m_ready_flag.store(true, std::memory_order_release);
        return;
    }

    // 在局部容器构建，全程不持锁；完成后一次性 swap，避免 search 长时间等锁
    std::vector<Entry> new_entries;
    new_entries.reserve(std::min(max_files, static_cast<size_t>(1000)));

    // BFS 遍历
    std::queue<fs::path> dir_queue;
    dir_queue.push(root);

    while (!dir_queue.empty() && new_entries.size() < max_files) {
        fs::path current_dir = dir_queue.front();
        dir_queue.pop();

        for (auto it = fs::directory_iterator(current_dir,
                                              fs::directory_options::skip_permission_denied, ec);
             it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                ec.clear();
                continue;
            }

            if (new_entries.size() >= max_files) {
                break;
            }

            const auto& entry = *it;
            std::string name = entry.path().filename().string();

            if (entry.is_directory(ec)) {
                // 跳过特定目录（如 .git/build/node_modules）
                if (!should_skip_dir(name)) {
                    dir_queue.push(entry.path());

                    // 目录也加入索引
                    Entry e;
                    e.name = name;
                    e.relative_path = fs::relative(entry.path(), root, ec).string();
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    std::replace(e.relative_path.begin(), e.relative_path.end(), '\\', '/');
                    e.modified = entry.last_write_time(ec);
                    if (ec) {
                        ec.clear();
                        e.modified = fs::file_time_type{};
                    }
                    e.is_directory = true;
                    new_entries.push_back(std::move(e));
                }
            } else if (entry.is_regular_file(ec)) {
                Entry e;
                e.name = name;
                e.relative_path = fs::relative(entry.path(), root, ec).string();
                if (ec) {
                    ec.clear();
                    continue;
                }

                // 规范化路径分隔符
                std::replace(e.relative_path.begin(), e.relative_path.end(), '\\', '/');

                e.modified = entry.last_write_time(ec);
                if (ec) {
                    ec.clear();
                    e.modified = fs::file_time_type{};
                }

                new_entries.push_back(std::move(e));
            }
        }
    }

    // 按修改时间倒序排列（最新在前）
    std::sort(new_entries.begin(), new_entries.end(),
              [](const Entry& a, const Entry& b) { return a.modified > b.modified; });

    // 短锁：一次性 swap 到 entries_，并更新元信息
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_ = std::move(new_entries);
        m_cwd_ = cwd;
        m_last_build_ts = std::chrono::steady_clock::now();
        m_dirty.store(false, std::memory_order_release);

        ready_ = true;
        // 就绪标志（is_ready / wait_ready 读取，不阻塞）
        m_ready_flag.store(true, std::memory_order_release);
    }
}

// ============================================================
// 搜索
// ============================================================

std::vector<FileIndex::Entry> FileIndex::search(const std::string& query, size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Entry> results;

    if (!ready_ || entries_.empty()) {
        return results;
    }

    // 空查询 → 返回最近修改的文件（最多 limit 个）
    if (query.empty()) {
        size_t count = std::min(limit, entries_.size());
        results.assign(entries_.begin(), entries_.begin() + count);
        return results;
    }

    // 非空查询 → 按文件名或路径子串匹配（大小写不敏感）
    std::string lower_query = to_lower(query);

    for (const auto& entry : entries_) {
        std::string lower_name = to_lower(entry.name);
        std::string lower_path = to_lower(entry.relative_path);

        if (lower_name.find(lower_query) != std::string::npos ||
            lower_path.find(lower_query) != std::string::npos) {
            results.push_back(entry);
            if (results.size() >= limit) {
                break;
            }
        }
    }

    return results;
}

std::vector<std::string> FileIndex::search_paths(const std::string& query, size_t limit) const {
    auto entries = search(query, limit);
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (auto& e : entries) {
        paths.push_back(std::move(e.relative_path));
    }
    return paths;
}

bool FileIndex::is_ready() const {
    // 读原子就绪标志，不碰 mutex_：后台 build 持锁期间调用不会阻塞 UI 线程
    return m_ready_flag.load(std::memory_order_acquire);
}

// ============================================================
// 异步构建（build_async / wait_ready / shutdown）
// ============================================================

void FileIndex::build_async(const std::string& cwd, size_t max_files) {
    // 已有后台构建在进行/已请求 → 跳过（重建防抖由 refresh_if_needed 负责）
    if (m_build_in_progress.exchange(true)) {
        return;
    }
    // 回收已完成的历史线程（exchange 成功说明旧线程已结束，join 立即返回）
    if (m_build_thread.joinable()) {
        m_build_thread.join();
    }
    m_build_thread = std::thread([this, cwd, max_files]() {
        build(cwd, max_files);  // 内部短暂持锁，完成后置 m_ready_flag
        m_build_in_progress.store(false, std::memory_order_release);
    });
}

bool FileIndex::wait_ready(int timeout_ms) const {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (m_ready_flag.load(std::memory_order_acquire)) {
            return true;
        }
        if (timeout_ms > 0 && std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void FileIndex::shutdown() {
    if (m_build_thread.joinable()) {
        m_build_thread.join();
    }
}

size_t FileIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

// ============================================================
// 按需刷新（方案 E：A + D 组合）
// ============================================================

void FileIndex::mark_dirty() {
    // 无锁原子操作，可安全在工具线程频繁调用
    m_dirty.store(true, std::memory_order_release);
}

bool FileIndex::refresh_if_needed(int64_t min_interval_ms) {
    // 加锁读取判断所需状态，释放锁后再调用 build（build 内部会加锁，避免递归死锁）
    std::string cwd;
    bool need_refresh = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_) {
            return false;  // 从未构建过，无法刷新
        }
        cwd = m_cwd_;

        const auto now = std::chrono::steady_clock::now();
        const auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_build_ts).count();
        const bool dirty = m_dirty.load(std::memory_order_acquire);
        need_refresh = dirty || elapsed_ms >= min_interval_ms;
    }

    if (need_refresh && !cwd.empty()) {
        // 后台线程重建，不阻塞 UI 线程（此前为同步 build，持锁 BFS 数百 ms 会卡界面）
        build_async(cwd);
        return true;
    }
    return false;
}

// ============================================================
// 全局单例
// ============================================================

FileIndex& global_file_index() {
    static FileIndex instance;
    return instance;
}

}  // namespace agent
