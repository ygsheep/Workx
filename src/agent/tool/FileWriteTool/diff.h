/**
 * @file diff.h
 * @brief 行级 diff 生成工具
 * @details 基于 LCS（最长公共子序列）算法的行级差异生成，
 *          输出 unified diff 风格文本，供 FileWriteTool/FileEditTool 反馈变更。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

namespace agent::tool {

/// @brief Diff 操作类型
enum class DiffOp {
    Equal,   ///< 未变更行
    Add,     ///< 新增行（仅在新文件中存在）
    Remove,  ///< 删除行（仅在旧文件中存在）
};

/// @brief 单个 diff 行
struct DiffLine {
    DiffOp op;         ///< 操作类型
    int old_line_no;   ///< 旧文件行号（1-based，0 表示无对应行）
    int new_line_no;   ///< 新文件行号（1-based，0 表示无对应行）
    std::string text;  ///< 行内容（不含换行符）
};

/// @brief 生成行级 diff
/// @details 使用 LCS 算法对比旧/新内容：
///          1. 按行分割 old_content / new_content
///          2. 构建 LCS 动态规划矩阵（O(n*m) 时间和空间）
///          3. 回溯生成 DiffLine 序列：LCS 内 → Equal；
///             仅旧有 → Remove；仅新增 → Add
/// @param old_content 旧文件内容（可能为空，表示纯新增）
/// @param new_content 新文件内容（可能为空，表示纯删除）
/// @return diff 行列表（按文件顺序排列）
std::vector<DiffLine> generate_line_diff(const std::string& old_content,
                                         const std::string& new_content);

/// @brief 格式化 diff 为 unified diff 风格文本
/// @details 输出格式：
/// @code
/// --- a/<file_path>
/// +++ b/<file_path>
/// @@ -10,3 +10,4 @@
///  int main() {
/// -    return 0;
/// +    std::cout << "Hello";
/// +    return 0;
///  }
/// @endcode
/// @param file_path 文件路径（用于 diff 头部）
/// @param diff_lines diff 行列表（来自 generate_line_diff）
/// @return 格式化后的 diff 文本；若无变更返回空字符串
std::string format_diff(const std::string& file_path, const std::vector<DiffLine>& diff_lines);

}  // namespace agent::tool
