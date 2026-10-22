/**
 * @file line_diff.h
 * @brief 行级 LCS diff（纯函数）
 * @details 输入旧/新内容按行，输出新文件行的对齐结果（Equal/Insert/Modify），
 *          不输出删除行——删除内容以修改点摘要呈现（侧边栏变更记录约定）。
 */

#pragma once

#include <string>
#include <vector>

namespace agent {

/// @brief 按行切分（兼容 \r\n / \n），空串返回空列表
std::vector<std::string> split_lines(const std::string& content);

/// @brief 行级 diff 单元（仅新文件坐标）
enum class DiffKind { Equal, Insert, Modify };

struct DiffLine {
    DiffKind kind = DiffKind::Equal;
    std::string text;
    int line_no = 0;  ///< 新文件坐标行号（1-based）
};

/// @brief 行级 LCS diff：old_lines → new_lines 的对齐结果
/// @details 仅输出新文件行（Equal/Insert/Modify），不输出删除行。
///          Modify = 该新行与对应旧行内容不同（替换/修改）。
///          行数超过阈值时降级为整块 Insert（大文件保护，O(n·m) 内存/时间）。
/// @param old_lines 旧内容按行（Write 全量改写时可为空 → 全部 Insert）
/// @param new_lines 新内容按行
/// @param new_start 新文件起始行号（1-based，默认 1）
std::vector<DiffLine> line_diff(const std::vector<std::string>& old_lines,
                                const std::vector<std::string>& new_lines, int new_start = 1);

}  // namespace agent
