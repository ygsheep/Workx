/**
 * @file diff.cpp
 * @brief 行级 diff 生成工具实现
 * @details LCS 动态规划算法 + unified diff 格式化。
 *          所有 filesystem 风格操作均不涉及，纯字符串处理。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/FileWriteTool/diff.h"

#include <sstream>
#include <algorithm>
#include <functional>

namespace agent::tool {

namespace {

/// @brief 将字符串按行分割
/// @details 按 '\n' 分割，移除行末 '\r'（CRLF 兼容）。
///          空字符串返回空列表；末尾换行不产生额外空行。
/// @param content 待分割的内容
/// @return 行列表（不含换行符）
std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    if (content.empty()) {
        return lines;
    }
    std::string current;
    current.reserve(128);
    for (char c : content) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(std::move(current));
            current.clear();
            current.reserve(128);
        } else {
            current.push_back(c);
        }
    }
    // 处理末尾无换行的最后一行
    if (!current.empty()) {
        if (current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(std::move(current));
    }
    return lines;
}

/// @brief LCS 长度阈值：超过此行数降级为全替换 diff，避免 OOM
/// @details 5000 行 × 5000 行 × 4 字节 ≈ 100 MB，仍可接受；
///          超过则降级（如 2.5 万行 → 2.5 GB 内存会 OOM）。
constexpr size_t LCS_MAX_LINES = 5000;

/// @brief 对每行计算 hash，LCS 比较时先用 hash 比较（O(1)），
///        hash 相同时再 fallback 到 string 比较，保证碰撞时正确性。
/// @details 大文件中相同行密集（如代码 import 区），string == 比较 O(len)
///          会累积显著开销；hash 比较可将相同行判断降至 O(1)。
std::vector<size_t> hash_lines(const std::vector<std::string>& lines) {
    std::vector<size_t> hashes;
    hashes.reserve(lines.size());
    const std::hash<std::string> hasher{};
    for (const auto& line : lines) {
        hashes.push_back(hasher(line));
    }
    return hashes;
}

/// @brief 构建 LCS 长度矩阵（一维数组紧凑存储）
/// @details dp[i*(n+1)+j] = old_lines[0..i) 与 new_lines[0..j) 的 LCS 长度。
///          矩阵大小 (old_size+1) * (new_size+1)，用一维 vector 替代 vector<vector<int>>
///          减少 inner vector 的开销与碎片。
/// @param old_lines 旧行列表
/// @param new_lines 新行列表
/// @return LCS 长度矩阵（一维存储，索引 [i][j] = i*(n+1)+j）
std::vector<int> build_lcs_table(const std::vector<std::string>& old_lines,
                                 const std::vector<std::string>& new_lines,
                                 const std::vector<size_t>& old_hashes,
                                 const std::vector<size_t>& new_hashes) {
    const size_t m = old_lines.size();
    const size_t n = new_lines.size();
    const size_t row_size = n + 1;
    std::vector<int> dp((m + 1) * row_size, 0);
    for (size_t i = 1; i <= m; ++i) {
        const size_t base_i = i * row_size;
        const size_t base_im1 = (i - 1) * row_size;
        const size_t hash_a = old_hashes[i - 1];
        const std::string& line_a = old_lines[i - 1];
        for (size_t j = 1; j <= n; ++j) {
            // hash 相同时再 fallback string 比较，避免碰撞误判
            bool equal = (hash_a == new_hashes[j - 1]);
            if (equal) {
                equal = (line_a == new_lines[j - 1]);
            }
            if (equal) {
                dp[base_i + j] = dp[base_im1 + (j - 1)] + 1;
            } else {
                dp[base_i + j] = std::max(dp[base_im1 + j], dp[base_i + (j - 1)]);
            }
        }
    }
    return dp;
}

/// @brief 回溯 LCS 矩阵生成 diff 序列
/// @details 从右下角回溯至左上角：
///          - old[i-1] == new[j-1] → Equal，i-- j--
///          - dp[i][j-1] >= dp[i-1][j] → Add(new[j-1])，j--
///          - 否则 → Remove(old[i-1])，i--
///          回溯结果逆序，需反转。
/// @param old_lines 旧行列表
/// @param new_lines 新行列表
/// @param dp LCS 长度矩阵（一维存储）
/// @param row_size 每行元素数 (n+1)
/// @return diff 行列表（按文件顺序）
std::vector<DiffLine> backtrack_diff(const std::vector<std::string>& old_lines,
                                     const std::vector<std::string>& new_lines,
                                     const std::vector<size_t>& old_hashes,
                                     const std::vector<size_t>& new_hashes,
                                     const std::vector<int>& dp, size_t row_size) {
    std::vector<DiffLine> diff;
    size_t i = old_lines.size();
    size_t j = new_lines.size();

    while (i > 0 || j > 0) {
        bool equal = false;
        if (i > 0 && j > 0) {
            // hash 先比较，相同再 fallback string
            if (old_hashes[i - 1] == new_hashes[j - 1]) {
                equal = (old_lines[i - 1] == new_lines[j - 1]);
            }
        }
        if (i > 0 && j > 0 && equal) {
            diff.push_back(
                {DiffOp::Equal, static_cast<int>(i), static_cast<int>(j), old_lines[i - 1]});
            --i;
            --j;
        } else if (j > 0 && (i == 0 || dp[i * row_size + (j - 1)] >= dp[(i - 1) * row_size + j])) {
            diff.push_back({DiffOp::Add, 0, static_cast<int>(j), new_lines[j - 1]});
            --j;
        } else {
            diff.push_back({DiffOp::Remove, static_cast<int>(i), 0, old_lines[i - 1]});
            --i;
        }
    }
    std::reverse(diff.begin(), diff.end());
    return diff;
}

}  // anonymous namespace

// ============================================================
// 公共 API 实现
// ============================================================

std::vector<DiffLine> generate_line_diff(const std::string& old_content,
                                         const std::string& new_content) {
    const auto old_lines = split_lines(old_content);
    const auto new_lines = split_lines(new_content);

    // 边界情况快速路径
    if (old_lines.empty() && new_lines.empty()) {
        return {};
    }
    if (old_lines.empty()) {
        std::vector<DiffLine> diff;
        diff.reserve(new_lines.size());
        for (size_t i = 0; i < new_lines.size(); ++i) {
            diff.push_back({DiffOp::Add, 0, static_cast<int>(i + 1), new_lines[i]});
        }
        return diff;
    }
    if (new_lines.empty()) {
        std::vector<DiffLine> diff;
        diff.reserve(old_lines.size());
        for (size_t i = 0; i < old_lines.size(); ++i) {
            diff.push_back({DiffOp::Remove, static_cast<int>(i + 1), 0, old_lines[i]});
        }
        return diff;
    }

    // 大文件降级：任一侧超过 LCS_MAX_LINES 行则跳过 LCS，生成全替换 diff
    // （LCS O(m*n) 内存，2.5 万行 ≈ 2.5 GB 会 OOM）
    if (old_lines.size() > LCS_MAX_LINES || new_lines.size() > LCS_MAX_LINES) {
        std::vector<DiffLine> diff;
        diff.reserve(old_lines.size() + new_lines.size());
        for (size_t i = 0; i < old_lines.size(); ++i) {
            diff.push_back({DiffOp::Remove, static_cast<int>(i + 1), 0, old_lines[i]});
        }
        for (size_t j = 0; j < new_lines.size(); ++j) {
            diff.push_back({DiffOp::Add, 0, static_cast<int>(j + 1), new_lines[j]});
        }
        return diff;
    }

    const auto old_hashes = hash_lines(old_lines);
    const auto new_hashes = hash_lines(new_lines);
    const auto dp = build_lcs_table(old_lines, new_lines, old_hashes, new_hashes);
    return backtrack_diff(old_lines, new_lines, old_hashes, new_hashes, dp, new_lines.size() + 1);
}

std::string format_diff(const std::string& file_path, const std::vector<DiffLine>& diff_lines) {
    if (diff_lines.empty()) {
        return {};
    }

    // 检查是否有变更（非 Equal 行）
    bool has_changes = false;
    for (const auto& line : diff_lines) {
        if (line.op != DiffOp::Equal) {
            has_changes = true;
            break;
        }
    }
    if (!has_changes) {
        return {};
    }

    std::ostringstream out;
    out << "--- a/" << file_path << "\n";
    out << "+++ b/" << file_path << "\n";

    // 生成 hunks：以连续 Equal 行作为分隔，每个变更块前保留 3 行上下文
    const int context_lines = 3;
    size_t idx = 0;
    while (idx < diff_lines.size()) {
        // 跳过前导 Equal 行，但保留最多 context_lines 行
        size_t hunk_start = idx;
        int old_start = 0;
        int new_start = 0;
        int old_count = 0;
        int new_count = 0;

        // 查找下一个变更行
        while (idx < diff_lines.size() && diff_lines[idx].op == DiffOp::Equal) {
            ++idx;
        }
        if (idx >= diff_lines.size()) {
            break;
        }

        // hunk 起点：回退 context_lines 行（但不越过已处理位置）
        size_t context_back = static_cast<size_t>(context_lines);
        if (context_back > (idx - hunk_start)) {
            context_back = idx - hunk_start;
        }
        size_t hunk_begin = idx - context_back;

        // 收集 hunk 直到遇到连续 (2*context_lines+1) 个 Equal 行或末尾
        size_t hunk_end = idx;
        size_t equal_run = 0;
        while (hunk_end < diff_lines.size()) {
            if (diff_lines[hunk_end].op == DiffOp::Equal) {
                ++equal_run;
                if (equal_run > 2 * context_lines + 1) {
                    break;
                }
            } else {
                equal_run = 0;
            }
            ++hunk_end;
        }
        // 末尾保留 context_lines 行 Equal
        if (equal_run > static_cast<size_t>(context_lines)) {
            hunk_end -= equal_run - context_lines;
        }

        // 计算 hunk 头部行号和计数
        for (size_t k = hunk_begin; k < hunk_end; ++k) {
            const auto& d = diff_lines[k];
            if (d.op == DiffOp::Equal) {
                if (old_start == 0) {
                    old_start = d.old_line_no;
                }
                if (new_start == 0) {
                    new_start = d.new_line_no;
                }
                ++old_count;
                ++new_count;
            } else if (d.op == DiffOp::Remove) {
                if (old_start == 0) {
                    old_start = d.old_line_no;
                }
                ++old_count;
            } else {  // Add
                if (new_start == 0) {
                    new_start = d.new_line_no;
                }
                ++new_count;
            }
        }

        // hunk 头部：@@ -old_start,old_count +new_start,new_count @@
        out << "@@ -" << old_start;
        if (old_count != 1) {
            out << "," << old_count;
        }
        out << " +" << new_start;
        if (new_count != 1) {
            out << "," << new_count;
        }
        out << " @@\n";

        // hunk 内容
        for (size_t k = hunk_begin; k < hunk_end; ++k) {
            const auto& d = diff_lines[k];
            char prefix = ' ';
            if (d.op == DiffOp::Add) {
                prefix = '+';
            } else if (d.op == DiffOp::Remove) {
                prefix = '-';
            }
            out << prefix << d.text << "\n";
        }

        idx = hunk_end;
    }

    return out.str();
}

}  // namespace agent::tool
