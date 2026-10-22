/**
 * @file line_endings.h
 * @brief 行尾风格检测与转换工具
 * @details 用于 FileEditTool 在编辑时保留原文件的行尾风格：
 *          - LF (\\n)      Unix / macOS
 *          - CRLF (\\r\\n)  Windows
 *          - CR (\\r)       旧 Mac（罕见，但为完整性保留）
 *
 *          工作流：
 *          1. 读取原始字节流
 *          2. detect_line_ending() 判定主性行尾风格
 *          3. normalize_to_lf() 将 CRLF/CR 规范化为 LF（用于匹配/替换）
 *          4. 在 LF 版本上做替换
 *          5. apply_line_ending() 将 LF 转回原风格（用于写回）
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>

namespace agent::tool {

/// @brief 行尾风格枚举
enum class LineEnding {
    LF,    ///< Unix / macOS (\\n)
    CRLF,  ///< Windows (\\r\\n)
    CR,    ///< 旧 Mac (\\r)
};

/// @brief 检测原始字节流的主性行尾风格
/// @details 统计 CRLF / 单独 LF / 单独 CR 出现次数，取多数。
///          平局优先级：CRLF > LF > CR（与 CC js-detect 行为一致）。
///          空内容或无换行符时返回 LF（默认）。
/// @param raw 原始字节流
/// @return 主性行尾风格
LineEnding detect_line_ending(std::string_view raw);

/// @brief 将 LF 规范化的内容转换为指定行尾风格
/// @details 输入必须仅含 LF（无 \\r），否则 \\r 会被保留导致混乱。
///          - LF:   原样返回
///          - CRLF: 每个 \\n 替换为 \\r\\n
///          - CR:   每个 \\n 替换为 \\r
/// @param lf_content LF 规范化后的内容
/// @param ending 目标行尾风格
/// @return 转换后的内容
std::string apply_line_ending(std::string_view lf_content, LineEnding ending);

/// @brief 将任意行尾风格的内容规范化为 LF
/// @details CRLF (\\r\\n) → LF (\\n)；孤立 \\r → LF。
///          保留末尾换行（与 FileEditTool 的 read_file_lf_normalized 一致）。
/// @param raw 原始字节流
/// @return LF 规范化后的内容
std::string normalize_to_lf(std::string_view raw);

/// @brief 行尾风格名称（用于日志/调试）
/// @param ending 行尾风格
/// @return "LF" / "CRLF" / "CR"
const char* line_ending_name(LineEnding ending);

}  // namespace agent::tool
