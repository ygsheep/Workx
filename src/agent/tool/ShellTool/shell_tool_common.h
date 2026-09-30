/**
 * @file shell_tool_common.h
 * @brief Shell 工具共享辅助函数（BashTool / PowerShellTool 共用）
 * @details 提取自 BashTool 的通用辅助逻辑：
 *          - 输出截断（truncate_output）
 *          - 空行清理（strip_empty_lines）
 *          - 结果格式化（format_result）
 *          - 共享常量（kMaxOutputChars / kDefaultTimeoutMs / kMaxTimeoutMs）
 *
 *          BashOutputRegistry 等工具专属逻辑不在此处，由各工具自行维护。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <format>
#include <sstream>
#include <string>

#include "core/process/exec_output.h"

namespace agent::tool::shell_common {

/// 默认超时：120 秒（对齐 cc BashTool 默认值）
constexpr int kDefaultTimeoutMs = 120'000;
/// 最大超时上限：600 秒（对齐 cc getMaxTimeoutMs）
constexpr int kMaxTimeoutMs = 600'000;
/// 输出截断阈值（对齐 ToolExecutor::MAX_TOOL_RESULT_LENGTH）
constexpr size_t kMaxOutputChars = 8'000;

/// @brief 截断输出到指定字符数，保留头尾
/// @details 对齐 ToolExecutor::finalize_result 的截断逻辑。
///          UTF-8 安全：截断点回退到字符边界，避免在多字节字符中间截断。
inline std::string truncate_output(std::string s, size_t max_chars = kMaxOutputChars) {
    if (s.size() <= max_chars) return s;
    const size_t head = max_chars / 2;
    const size_t tail = max_chars - head;

    // UTF-8 安全截断：回退到非 continuation byte
    auto safe_pos = [](const std::string& str, size_t pos) -> size_t {
        if (pos >= str.size()) return str.size();
        while (pos > 0 && (static_cast<unsigned char>(str[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        return pos;
    };

    const size_t head_end = safe_pos(s, head);
    const size_t tail_start = safe_pos(s, s.size() - tail);
    const size_t omitted = s.size() - head_end - (s.size() - tail_start);
    return s.substr(0, head_end) +
           std::format("\n... [output truncated, {} characters omitted] ...\n", omitted) +
           s.substr(tail_start);
}

/// @brief 去除连续空行（对齐 cc stripEmptyLines）
/// @details L-1 修复：末尾仅保留单个换行，避免 </stdout> 前出现空行
inline std::string strip_empty_lines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool last_was_blank = false;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (blank && last_was_blank) continue;
        out += line;
        out += '\n';
        last_was_blank = blank;
    }
    // 去掉末尾多余的连续换行，只保留一个
    while (out.size() >= 2 && out.back() == '\n' && out[out.size() - 2] == '\n') {
        out.pop_back();
    }
    return out;
}

/// @brief 格式化执行结果为 LLM 可读文本
/// @details 统一输出格式：<error> / <stdout> / <stderr> 标签
inline std::string format_result(const process::ExecOutput& out) {
    std::ostringstream ss;
    if (out.timed_out) {
        ss << "<error>Command timed out</error>\n";
    } else if (out.cancelled) {
        ss << "<error>Command was cancelled</error>\n";
    } else if (out.exit_code != 0) {
        ss << std::format("<error>Command exited with code {}</error>\n", out.exit_code);
    }

    if (!out.stdout_text.empty()) {
        ss << "<stdout>\n" << strip_empty_lines(out.stdout_text) << "</stdout>\n";
    }
    if (!out.stderr_text.empty()) {
        ss << "<stderr>\n" << strip_empty_lines(out.stderr_text) << "</stderr>\n";
    }
    if (out.stdout_text.empty() && out.stderr_text.empty()) {
        ss << "(no output)\n";
    }
    return ss.str();
}

}  // namespace agent::tool::shell_common
