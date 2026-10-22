/**
 * @file line_endings.cpp
 * @brief 行尾风格检测与转换工具实现
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/line_endings.h"

namespace agent::tool {

LineEnding detect_line_ending(std::string_view raw) {
    size_t crlf_count = 0;  // \r\n
    size_t lf_count = 0;    // 单独 \n（前面不是 \r）
    size_t cr_count = 0;    // 单独 \r（后面不是 \n）

    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++crlf_count;
                ++i;  // 跳过 \n
            } else {
                ++cr_count;
            }
        } else if (raw[i] == '\n') {
            ++lf_count;
        }
    }

    // 平局优先级：CRLF > LF > CR
    if (crlf_count >= lf_count && crlf_count >= cr_count && crlf_count > 0) {
        return LineEnding::CRLF;
    }
    if (lf_count >= cr_count && lf_count > 0) {
        return LineEnding::LF;
    }
    if (cr_count > 0) {
        return LineEnding::CR;
    }
    // 空内容或无换行符：默认 LF
    return LineEnding::LF;
}

std::string apply_line_ending(std::string_view lf_content, LineEnding ending) {
    if (ending == LineEnding::LF) {
        return std::string(lf_content);
    }

    std::string result;
    result.reserve(lf_content.size() * 2);  // 最坏情况 CRLF 翻倍

    for (size_t i = 0; i < lf_content.size(); ++i) {
        if (lf_content[i] == '\n') {
            if (ending == LineEnding::CRLF) {
                result += "\r\n";
            } else {  // CR
                result += '\r';
            }
        } else {
            result += lf_content[i];
        }
    }
    return result;
}

std::string normalize_to_lf(std::string_view raw) {
    std::string normalized;
    normalized.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\r') {
            normalized += '\n';
            if (i + 1 < raw.size() && raw[i + 1] == '\n') {
                ++i;
            }
        } else {
            normalized += raw[i];
        }
    }
    return normalized;
}

const char* line_ending_name(LineEnding ending) {
    switch (ending) {
        case LineEnding::LF:
            return "LF";
        case LineEnding::CRLF:
            return "CRLF";
        case LineEnding::CR:
            return "CR";
    }
    return "LF";
}

}  // namespace agent::tool
