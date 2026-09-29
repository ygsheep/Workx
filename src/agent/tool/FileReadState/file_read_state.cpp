/**
 * @file file_read_state.cpp
 * @brief FileReadStateTracker 实现
 * @details 单例注册表的线程安全实现。所有方法通过 mutex 保护。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/FileReadState/file_read_state.h"

#include <algorithm>

namespace agent::tool {

namespace {

/// @brief 将新范围合并进有序范围集合（插入 + 相邻/重叠合并）
/// @details 范围按 start 升序、互不重叠（相邻范围合并为一个）。
/// @param ranges 有序范围集合（原地修改）
/// @param r 待插入的范围
void merge_range(std::vector<LineRange>& ranges, const LineRange& r) {
    if (r.end < r.start) return;  // 空范围

    std::vector<LineRange> merged;
    bool inserted = false;

    for (const auto& cur : ranges) {
        // 当前范围整体在新范围之前
        if (cur.end + 1 < r.start) {
            merged.push_back(cur);
            continue;
        }
        // 当前范围整体在新范围之后（且未插入过）
        if (cur.start > r.end + 1) {
            if (!inserted) {
                merged.push_back(r);
                inserted = true;
            }
            merged.push_back(cur);
            continue;
        }
        // 与当前范围重叠或相邻：扩展新范围
        if (!inserted) {
            merged.push_back({std::min(cur.start, r.start), std::max(cur.end, r.end)});
            inserted = true;
        } else {
            // 与已插入的合并范围再次重叠（罕见，防御性处理）
            auto& last = merged.back();
            last.start = std::min(last.start, cur.start);
            last.end = std::max(last.end, cur.end);
        }
    }

    if (!inserted) merged.push_back(r);
    ranges = std::move(merged);
}

}  // namespace

// ============================================================
// 公共 API
// ============================================================

void FileReadStateTracker::record_read(const std::string& canonical_path, std::string content,
                                       std::filesystem::file_time_type mtime, bool is_partial_view,
                                       int32_t offset, int32_t lines_read, int32_t total_lines) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& state = m_states[canonical_path];
    state.mtime = mtime;
    state.content = std::move(content);
    state.is_partial_view = is_partial_view;
    state.total_lines = total_lines;

    // 完整读取且已知总行数：视为覆盖全文件
    if (!is_partial_view && total_lines > 0) {
        state.read_ranges = {{1, total_lines}};
    } else if (lines_read > 0) {
        // 部分读取：合并本次范围（跨多次读取累积）
        merge_range(state.read_ranges, {offset, offset + lines_read - 1});
    }
}

std::optional<FileReadState> FileReadStateTracker::get_state(
    const std::string& canonical_path) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_states.find(canonical_path);
    if (it == m_states.end()) {
        return std::nullopt;
    }
    return it->second;
}

void FileReadStateTracker::update_after_write(const std::string& canonical_path,
                                              std::string new_content,
                                              std::filesystem::file_time_type new_mtime,
                                              bool is_partial_view) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& state = m_states[canonical_path];
    state.mtime = new_mtime;
    state.content = std::move(new_content);
    state.is_partial_view = is_partial_view;

    // 写入后视为完整视图：从新内容计算总行数，read_ranges 覆盖全文件
    state.total_lines = 1;
    for (char c : state.content) {
        if (c == '\n') ++state.total_lines;
    }
    state.read_ranges = {{1, state.total_lines}};
}

void FileReadStateTracker::remove(const std::string& canonical_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states.erase(canonical_path);
}

void FileReadStateTracker::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states.clear();
}

void FileReadStateTracker::clear_for_test() { clear(); }

size_t FileReadStateTracker::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_states.size();
}

}  // namespace agent::tool
