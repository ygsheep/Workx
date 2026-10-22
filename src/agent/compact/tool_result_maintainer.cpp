/**
 * @file tool_result_maintainer.cpp
 * @brief tool_result 两级维护实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/compact/tool_result_maintainer.h"

#include <algorithm>
#include <sstream>
#include <format>

namespace agent::compact {

namespace {

/// @brief 按行分割文本
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;
    std::string current;
    current.reserve(text.size());
    for (char c : text) {
        if (c == '\n') {
            lines.push_back(std::move(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    // 最后一段（无尾随 \n）
    if (!current.empty() || (!text.empty() && text.back() == '\n')) {
        lines.push_back(std::move(current));
    }
    return lines;
}

/// @brief 判定 tool_result 是否已被 snip/prune 过
bool already_elided(const std::string& content) {
    return content.find("[snipped") != std::string::npos ||
           content.find("[elided") != std::string::npos;
}

}  // anonymous namespace

// ============================================================
// 默认策略
// ============================================================

const SnipStrategy& default_read_only_snip() {
    static const SnipStrategy s{80, 12, 10'000, 2'000};
    return s;
}

const SnipStrategy& default_side_effecting_snip() {
    static const SnipStrategy s{40, 40, 8'000, 8'000};
    return s;
}

// ============================================================
// 工具分类
// ============================================================

bool is_read_only_tool(const std::string& tool_name) {
    // 已知副作用工具列表（白名单反转：默认只读，副作用工具显式列出）
    // 对齐 tool_result_maintainer.h 文档：未知工具按只读处理（默认头长尾短）
    static const std::vector<std::string> side_effecting = {
        "Write",      "FileWrite",    "file_write", "Edit",       "FileEdit", "file_edit",
        "Bash",       "bash",         "PowerShell", "powershell", "Agent",    "TaskCreate",
        "TaskUpdate", "TaskComplete", "TodoWrite",  "todo_write"};
    // 已知副作用工具返回 false；未知工具默认 true（保守策略，避免误截短）
    return std::find(side_effecting.begin(), side_effecting.end(), tool_name) ==
           side_effecting.end();
}

const SnipStrategy& select_strategy_by_tool_name(const std::string& tool_name) {
    return is_read_only_tool(tool_name) ? default_read_only_snip() : default_side_effecting_snip();
}

// ============================================================
// snip_tool_result — 截短单条 tool_result
// ============================================================

int snip_tool_result(ChatMessage& msg, const SnipStrategy& strategy) {
    if (msg.role != ChatMessage::Role::Tool) return 0;
    if (msg.content.empty()) return 0;
    if (already_elided(msg.content)) return 0;

    const size_t original_len = msg.content.size();

    auto lines = split_lines(msg.content);
    const size_t total_lines = lines.size();

    // 行数不足：尝试按字符截短（fallback）
    if (total_lines <= static_cast<size_t>(strategy.head_lines + strategy.tail_lines)) {
        // 按 head_chars / tail_chars 截短
        if (original_len <= static_cast<size_t>(strategy.head_chars + strategy.tail_chars + 100)) {
            // 内容已经足够短，不截短
            return 0;
        }
        // 字符级截短
        std::string head =
            msg.content.substr(0, std::min<size_t>(msg.content.size(), strategy.head_chars));
        std::string tail;
        if (msg.content.size() > static_cast<size_t>(strategy.head_chars + strategy.tail_chars)) {
            tail = msg.content.substr(msg.content.size() - strategy.tail_chars);
        }
        size_t elided = msg.content.size() - head.size() - tail.size();
        if (elided <= 0) return 0;

        std::string elided_str = std::format("[snipped tool result — {} chars elided]\n", elided);
        msg.content = head + "\n... " + elided_str + "...\n" + tail;
        return static_cast<int>(original_len - msg.content.size());
    }

    // 行级截短：保留头 N 行 + 尾 M 行
    std::ostringstream oss;
    int head_n = std::max(1, strategy.head_lines);
    int tail_n = std::max(1, strategy.tail_lines);

    // 头部
    for (int i = 0; i < head_n && static_cast<size_t>(i) < total_lines; ++i) {
        oss << lines[i] << "\n";
    }

    // 中段占位符
    size_t elided_lines = total_lines - head_n - tail_n;
    oss << std::format("[snipped tool result — {} lines elided]\n", elided_lines);

    // 尾部
    for (int i = 0; i < tail_n; ++i) {
        size_t idx = total_lines - tail_n + i;
        if (idx < total_lines) {
            oss << lines[idx] << "\n";
        }
    }

    msg.content = oss.str();
    int saved = static_cast<int>(original_len) - static_cast<int>(msg.content.size());
    return saved > 0 ? saved : 0;
}

// ============================================================
// prune_tool_result — 删除单条 tool_result 内容
// ============================================================

int prune_tool_result(ChatMessage& msg, const std::string& archive_dir) {
    if (msg.role != ChatMessage::Role::Tool) return 0;
    if (msg.content.empty()) return 0;
    if (already_elided(msg.content)) return 0;

    const size_t original_len = msg.content.size();
    std::string elided_str =
        std::format("[elided tool result — {} bytes{}]\n", original_len,
                    archive_dir.empty() ? "" : (", archived to " + archive_dir));
    msg.content = std::move(elided_str);
    int saved = static_cast<int>(original_len) - static_cast<int>(msg.content.size());
    return saved > 0 ? saved : 0;
}

// ============================================================
// 批量操作
// ============================================================

SnipStats snip_range(std::vector<ChatMessage>& messages, size_t head_end, size_t tail_start,
                     const std::string& archive_dir) {
    SnipStats stats;
    if (head_end >= tail_start || tail_start > messages.size()) {
        return stats;
    }
    for (size_t i = head_end; i < tail_start && i < messages.size(); ++i) {
        auto& msg = messages[i];
        if (msg.role != ChatMessage::Role::Tool) continue;
        if (msg.content.empty()) continue;
        if (already_elided(msg.content)) continue;

        const SnipStrategy& strategy = select_strategy_by_tool_name(msg.tool_name);
        int saved = snip_tool_result(msg, strategy);
        if (saved > 0) {
            stats.results++;
            stats.saved_chars += saved;
        }
    }
    stats.archive = archive_dir;
    return stats;
}

SnipStats prune_range(std::vector<ChatMessage>& messages, size_t head_end, size_t tail_start,
                      const std::string& archive_dir) {
    SnipStats stats;
    if (head_end >= tail_start || tail_start > messages.size()) {
        return stats;
    }
    for (size_t i = head_end; i < tail_start && i < messages.size(); ++i) {
        auto& msg = messages[i];
        if (msg.role != ChatMessage::Role::Tool) continue;
        if (msg.content.empty()) continue;
        if (already_elided(msg.content)) continue;

        int saved = prune_tool_result(msg, archive_dir);
        if (saved > 0) {
            stats.results++;
            stats.saved_chars += saved;
        }
    }
    stats.archive = archive_dir;
    return stats;
}

}  // namespace agent::compact
