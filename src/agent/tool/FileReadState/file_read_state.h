/**
 * @file file_read_state.h
 * @brief FileReadStateTracker — 文件读取状态追踪器
 * @details 单例，记录 FileReadTool 读取过的文件路径、mtime、内容快照、是否部分读取。
 *          供 FileWriteTool/FileEditTool 做 pre-read 强制检查与 staleness 检测。
 *          对齐 Claude Code `readFileState` 设计（ToolUseContext.readFileState）。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <optional>
#include <vector>
#include <filesystem>

namespace agent::tool {

/// @brief 已读行范围（1-based 闭区间）
struct LineRange {
    int32_t start = 0;  ///< 起始行（含）
    int32_t end = 0;    ///< 结束行（含）

    [[nodiscard]] bool contains(int32_t line) const { return line >= start && line <= end; }
};

/// @brief 文件读取状态快照
///
/// 由 FileReadTool 在成功读取后填充，供 FileWriteTool/FileEditTool 校验：
/// - `mtime`：读取时的文件最后修改时间，用于 staleness 快速检测
/// - `content`：LF 规范化后的内容快照，用于 mtime 变化后的内容对比回退
/// - `is_partial_view`：是否使用 offset/limit 部分读取（部分读取不可做内容对比）
/// - `read_ranges`：已读取的行范围集合（有序、不重叠、已合并），
///   供 FileEditTool 按"编辑目标是否在已读范围内"做局部校验
struct FileReadState {
    std::filesystem::file_time_type mtime;  ///< 读取时的 mtime
    std::string content;                    ///< LF 规范化内容快照
    bool is_partial_view{false};            ///< 是否部分读取（offset/limit）
    std::vector<LineRange> read_ranges;     ///< 已读行范围（1-based）
    int32_t total_lines = 0;                ///< 文件总行数（未知时 0）

    /// @brief 判断指定行（1-based）是否已被读取
    [[nodiscard]] bool covers_line(int32_t line) const {
        for (const auto& r : read_ranges) {
            if (r.contains(line)) return true;
            if (r.start > line) break;  // read_ranges 有序，提前退出
        }
        return false;
    }
};

/// @brief FileReadStateTracker — 文件读取状态追踪器（单例）
///
/// 跨工具共享的文件读取状态注册表：
/// - FileReadTool 在 `call()` 成功后调用 `record_read()` 记录状态
/// - FileWriteTool 在 `call()` 中查询状态做 pre-read + staleness 检查
/// - FileWriteTool 写入成功后调用 `update_after_write()` 刷新状态
///
/// @par 线程安全
/// 所有公共方法通过内部 mutex 保护，可跨线程安全调用。
///
/// @par 路径规范化
/// 调用方应传入 `fs::weakly_canonical(path).generic_string()` 作为 key，
/// 保证不同相对路径 / 平台分隔符均映射到同一 entry。
///
/// @par 与 Claude Code 对齐
/// 对应 CC `ToolUseContext.readFileState: Map<string, ReadFileState>`，
/// 其中 `ReadFileState = { content, timestamp, offset, limit, isPartialView }`。
class FileReadStateTracker final {
   public:
    /// @brief 获取单例
    /// @return 单例引用
    static FileReadStateTracker& instance() noexcept {
        static FileReadStateTracker inst;
        return inst;
    }

    FileReadStateTracker(const FileReadStateTracker&) = delete;
    FileReadStateTracker& operator=(const FileReadStateTracker&) = delete;
    FileReadStateTracker(FileReadStateTracker&&) = delete;
    FileReadStateTracker& operator=(FileReadStateTracker&&) = delete;

    /// @brief 记录文件读取状态
    /// @details 由 FileReadTool 在成功读取后调用。相同路径合并读取范围（不覆盖）。
    /// @param canonical_path 规范化路径（generic_string 形式）
    /// @param content LF 规范化后的内容快照
    /// @param mtime 读取时的文件 mtime（fs::last_write_time 返回值）
    /// @param is_partial_view 是否部分读取（offset/limit）
    /// @param offset 本次读取的起始行（1-based，默认 1）
    /// @param lines_read 本次实际读取的行数（默认 0）
    /// @param total_lines 文件总行数（未知时 0；完整读取且 >0 时视为覆盖全文件）
    void record_read(const std::string& canonical_path, std::string content,
                     std::filesystem::file_time_type mtime, bool is_partial_view = false,
                     int32_t offset = 1, int32_t lines_read = 0, int32_t total_lines = 0);

    /// @brief 查询文件读取状态
    /// @param canonical_path 规范化路径
    /// @return 状态快照（若存在），否则 nullopt
    [[nodiscard]] std::optional<FileReadState> get_state(const std::string& canonical_path) const;

    /// @brief 写入后刷新状态
    /// @details 由 FileWriteTool/FileEditTool 在成功写入后调用，使后续连续写入通过 staleness 检查。
    ///          写入后视为完整视图：read_ranges 重置为覆盖全文件。
    /// @param canonical_path 规范化路径
    /// @param new_content 新内容（LF 规范化）
    /// @param new_mtime 写入后的 mtime（重新 stat 获取）
    /// @param is_partial_view 是否部分视图（一般为 false，因为写入后视为完整视图）
    void update_after_write(const std::string& canonical_path, std::string new_content,
                            std::filesystem::file_time_type new_mtime,
                            bool is_partial_view = false);

    /// @brief 清除指定路径的状态
    /// @param canonical_path 规范化路径
    void remove(const std::string& canonical_path);

    /// @brief 清除所有状态
    void clear();

    /// @brief 清除所有状态（测试专用，语义同 clear）
    void clear_for_test();

    /// @brief 获取已追踪的文件数量
    /// @return 条目数
    [[nodiscard]] size_t size() const;

   private:
    FileReadStateTracker() = default;
    ~FileReadStateTracker() = default;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, FileReadState> m_states;
};

}  // namespace agent::tool
