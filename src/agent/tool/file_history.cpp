/**
 * @file file_history.cpp
 * @brief 文件历史管理器实现
 * @details 提供会话内多版本备份能力：
 *          - save_version: 写入前保存当前内容快照
 *          - get_versions/get_version/get_latest_version: 查询历史
 *          - clear_versions: 清除指定文件历史
 *          - 自动裁剪：超过 max_versions_ 时移除最旧版本
 *          线程安全：所有公共方法加锁保护 history_。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/file_history.h"

#include <algorithm>
#include <format>
#include <sstream>

namespace agent::tool {

// ============================================================
// 单例
// ============================================================

FileHistory& FileHistory::instance() {
    static FileHistory inst;
    return inst;
}

FileHistory::FileHistory() = default;

// ============================================================
// 公共接口实现
// ============================================================

std::string FileHistory::save_version(const std::string& file_path, const std::string& content,
                                      const std::string& operation) {
    std::lock_guard<std::mutex> lock(mutex_);

    const auto now = std::chrono::system_clock::now();
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    // 生成版本 ID：<timestamp_ms>_<seq>
    const std::string version_id = std::format("{}_{}", ms, next_id_++);

    FileVersion version{
        .version_id = version_id, .timestamp = now, .content = content, .operation = operation};

    // 插入到列表头部（最新在前）
    auto& versions = history_[file_path];
    versions.insert(versions.begin(), std::move(version));

    // 裁剪旧版本
    prune_old_versions_locked(file_path);

    return version_id;
}

std::vector<FileVersion> FileHistory::get_versions(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(file_path);
    if (it == history_.end()) return {};
    return it->second;
}

std::optional<FileVersion> FileHistory::get_version(const std::string& file_path,
                                                    const std::string& version_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(file_path);
    if (it == history_.end()) return std::nullopt;

    for (const auto& v : it->second) {
        if (v.version_id == version_id) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<FileVersion> FileHistory::get_latest_version(const std::string& file_path) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = history_.find(file_path);
    if (it == history_.end() || it->second.empty()) return std::nullopt;
    return it->second.front();
}

void FileHistory::clear_versions(const std::string& file_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.erase(file_path);
}

void FileHistory::clear_for_test() {
    std::lock_guard<std::mutex> lock(mutex_);
    history_.clear();
    next_id_ = 0;
    max_versions_ = 20;
}

// ============================================================
// 私有辅助
// ============================================================

void FileHistory::prune_old_versions_locked(const std::string& file_path) {
    auto it = history_.find(file_path);
    if (it == history_.end()) return;

    auto& versions = it->second;
    if (versions.size() <= max_versions_) return;

    // 移除最旧的版本（列表尾部）
    const size_t to_remove = versions.size() - max_versions_;
    versions.erase(versions.end() - static_cast<std::ptrdiff_t>(to_remove), versions.end());
}

}  // namespace agent::tool
