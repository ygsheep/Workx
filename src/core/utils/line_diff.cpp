/**
 * @file line_diff.cpp
 * @brief 行级 LCS diff 实现（仅输出新文件行）
 * @details 与 FileWriteTool 的 generate_line_diff 不同，本实现面向 UI 内联高亮：
 *          只输出新文件行（Equal/Insert/Modify），删除行折叠为 Modify。
 *          大文件（任一侧 > kLcsMaxLines）降级为全 Insert，避免 O(n·m) 内存爆炸。
 */

#include "core/utils/line_diff.h"

#include <algorithm>
#include <functional>

namespace agent {

std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> lines;
    if (content.empty()) return lines;
    std::string cur;
    for (const char c : content) {
        if (c == '\n') {
            lines.push_back(std::move(cur));
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty()) lines.push_back(std::move(cur));
    return lines;
}

namespace {

/// @brief LCS 行数阈值：超过则跳过 LCS 直接全 Insert（O(n·m) 内存保护）
constexpr std::size_t kLcsMaxLines = 5000;

/// @brief 内部原始 diff 操作（含删除行，供后续折叠为 Modify）
enum class RawOp { Equal, Remove, Add };

struct RawLine {
    RawOp op = RawOp::Equal;
    int new_line_no = 0;  ///< 新文件行号（1-based；Add/Equal 有效，Remove 为 0）
    std::string text;
};

/// @brief 每行 hash，LCS 比较先比 hash（O(1)），相同再 fallback 字符串比较防碰撞
std::vector<std::size_t> hash_lines(const std::vector<std::string>& lines) {
    std::vector<std::size_t> hashes;
    hashes.reserve(lines.size());
    const std::hash<std::string> hasher{};
    for (const auto& line : lines) hashes.push_back(hasher(line));
    return hashes;
}

/// @brief 标准 LCS diff（Equal/Remove/Add），回溯生成按文件顺序的序列
std::vector<RawLine> raw_diff(const std::vector<std::string>& old_lines,
                              const std::vector<std::string>& new_lines) {
    const std::size_t m = old_lines.size();
    const std::size_t n = new_lines.size();

    if (m == 0 && n == 0) return {};
    if (m == 0) {
        std::vector<RawLine> out;
        out.reserve(n);
        for (std::size_t j = 0; j < n; ++j)
            out.push_back({RawOp::Add, static_cast<int>(j + 1), new_lines[j]});
        return out;
    }
    if (n == 0) {
        std::vector<RawLine> out;
        out.reserve(m);
        for (std::size_t i = 0; i < m; ++i) out.push_back({RawOp::Remove, 0, old_lines[i]});
        return out;
    }
    if (m > kLcsMaxLines || n > kLcsMaxLines) {
        // 大文件降级：整块 Insert（不输出删除行，避免 O(n·m) 内存爆炸）
        std::vector<RawLine> out;
        out.reserve(n);
        for (std::size_t j = 0; j < n; ++j)
            out.push_back({RawOp::Add, static_cast<int>(j + 1), new_lines[j]});
        return out;
    }

    const auto old_hashes = hash_lines(old_lines);
    const auto new_hashes = hash_lines(new_lines);
    const std::size_t row_size = n + 1;
    std::vector<int> dp((m + 1) * row_size, 0);
    for (std::size_t i = 1; i <= m; ++i) {
        const std::size_t base_i = i * row_size;
        const std::size_t base_im1 = (i - 1) * row_size;
        const std::size_t ha = old_hashes[i - 1];
        const std::string& la = old_lines[i - 1];
        for (std::size_t j = 1; j <= n; ++j) {
            bool eq = (ha == new_hashes[j - 1]);
            if (eq) eq = (la == new_lines[j - 1]);
            if (eq)
                dp[base_i + j] = dp[base_im1 + (j - 1)] + 1;
            else
                dp[base_i + j] = std::max(dp[base_im1 + j], dp[base_i + (j - 1)]);
        }
    }

    std::vector<RawLine> out;
    std::size_t i = m, j = n;
    while (i > 0 || j > 0) {
        bool eq = false;
        if (i > 0 && j > 0 && old_hashes[i - 1] == new_hashes[j - 1])
            eq = (old_lines[i - 1] == new_lines[j - 1]);
        if (i > 0 && j > 0 && eq) {
            out.push_back({RawOp::Equal, static_cast<int>(j), old_lines[i - 1]});
            --i;
            --j;
        } else if (j > 0 && (i == 0 || dp[i * row_size + (j - 1)] >= dp[(i - 1) * row_size + j])) {
            out.push_back({RawOp::Add, static_cast<int>(j), new_lines[j - 1]});
            --j;
        } else {
            out.push_back({RawOp::Remove, 0, old_lines[i - 1]});
            --i;
        }
    }
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace

std::vector<DiffLine> line_diff(const std::vector<std::string>& old_lines,
                                const std::vector<std::string>& new_lines, int new_start) {
    const auto raw = raw_diff(old_lines, new_lines);

    std::vector<DiffLine> out;
    out.reserve(new_lines.size());

    // 删除行折叠：连续 Remove 后跟的 Add 配对为 Modify（一一对应），
    // 多余 Remove 直接丢弃（删除内容以修改点摘要呈现，不渲染删除行）
    int pending_removes = 0;
    for (const auto& r : raw) {
        switch (r.op) {
            case RawOp::Equal:
                pending_removes = 0;
                out.push_back({DiffKind::Equal, r.text, new_start + r.new_line_no - 1});
                break;
            case RawOp::Remove:
                ++pending_removes;
                break;
            case RawOp::Add:
                if (pending_removes > 0) {
                    --pending_removes;
                    out.push_back({DiffKind::Modify, r.text, new_start + r.new_line_no - 1});
                } else {
                    out.push_back({DiffKind::Insert, r.text, new_start + r.new_line_no - 1});
                }
                break;
        }
    }
    return out;
}

}  // namespace agent
