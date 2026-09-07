/**
 * @file quote_normalizer.cpp
 * @brief 引号规范化工具实现（对齐 CC findActualString + preserveQuoteStyle）
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/quote_normalizer.h"

#include <algorithm>
#include <vector>

namespace agent::tool {

namespace {

/// UTF-8 字节常量（弯引号）
constexpr char kSmartSingleLeft[]  = "\xE2\x80\x98";  // U+2018 '
constexpr char kSmartSingleRight[] = "\xE2\x80\x99";  // U+2019 '
constexpr char kSmartDoubleLeft[]  = "\xE2\x80\x9C";  // U+201C "
constexpr char kSmartDoubleRight[] = "\xE2\x80\x9D";  // U+201D "

/// 检查字节序列是否匹配指定前缀
[[maybe_unused]] bool starts_with(std::string_view s, size_t pos, std::string_view prefix) {
    if (pos + prefix.size() > s.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (s[pos + i] != prefix[i]) return false;
    }
    return true;
}

/// @brief 规范化内容 + 原始字节偏移映射
/// @details offsets[i] = normalized[i] 在原始字符串中的字节偏移
///          offsets.size() == normalized.size() + 1（末尾追加 end marker）
struct NormalizedWithOffsets {
    std::string normalized;
    std::vector<size_t> offsets;
};

NormalizedWithOffsets normalize_with_offsets(std::string_view original) {
    NormalizedWithOffsets result;
    result.normalized.reserve(original.size());
    result.offsets.reserve(original.size() + 1);

    size_t i = 0;
    while (i < original.size()) {
        result.offsets.push_back(i);

        // 检测弯引号（3 字节 E2 80 98/99/9C/9D）
        if (i + 2 < original.size() && original[i] == '\xE2' && original[i + 1] == '\x80') {
            char c = original[i + 2];
            if (c == '\x98' || c == '\x99') {
                result.normalized += '\'';
                i += 3;
                continue;
            }
            if (c == '\x9C' || c == '\x9D') {
                result.normalized += '"';
                i += 3;
                continue;
            }
        }
        // 其他字符原样保留（含 em dash U+2014 / en dash U+2013 等）
        result.normalized += original[i];
        ++i;
    }
    result.offsets.push_back(original.size());  // end marker
    return result;
}

/// @brief 判断位置 i 处的引号是否为 opening context（使用左引号）
/// @details 对齐 CC isOpeningContext：
///          - i == 0 → true
///          - 前一字符为 space/tab/\\n/\\r/(/[/{/em dash/en dash → true
///          - 否则 → false
/// @param s UTF-8 字符串
/// @param i 当前位置（字节偏移）
/// @return true 表示应使用左引号
bool is_opening_context(std::string_view s, size_t i) {
    if (i == 0) return true;

    // 找到前一个 UTF-8 字符的起始字节
    size_t prev = i - 1;
    // 回溯到前一个字符的起始字节（UTF-8 后续字节以 10xxxxxx 开头）
    while (prev > 0 && (static_cast<unsigned char>(s[prev]) & 0xC0) == 0x80) {
        --prev;
    }

    char c = s[prev];
    // ASCII 判断
    switch (c) {
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case '(':
        case '[':
        case '{':
            return true;
        default:
            break;
    }

    // em dash (U+2014 = E2 80 94) / en dash (U+2013 = E2 80 93)
    if (prev + 2 < s.size() && s[prev] == '\xE2' && s[prev + 1] == '\x80') {
        char dash = s[prev + 2];
        if (dash == '\x94' || dash == '\x93') {
            return true;
        }
    }
    return false;
}

/// @brief 判断位置 i 处是否为缩写形式（apostrophe）
/// @details 两侧均为字母时返回 true（如 "don't" 中的 '）
bool is_apostrophe_context(std::string_view s, size_t i) {
    if (i == 0 || i + 1 >= s.size()) return false;

    // 前一字符必须是字母
    size_t prev = i - 1;
    while (prev > 0 && (static_cast<unsigned char>(s[prev]) & 0xC0) == 0x80) {
        --prev;
    }
    // 简单 ASCII 字母判断（A-Z, a-z）
    char pc = s[prev];
    bool prev_is_alpha = (pc >= 'A' && pc <= 'Z') || (pc >= 'a' && pc <= 'z');

    // 后一字符必须是字母
    size_t next = i + 1;
    char nc = s[next];
    bool next_is_alpha = (nc >= 'A' && nc <= 'Z') || (nc >= 'a' && nc <= 'z');

    // 对于 UTF-8 多字节字母（如带重音字母），这里简化为 false
    // CC 使用 \p{L}u，Workx 暂不支持完整 Unicode 类别判断
    return prev_is_alpha && next_is_alpha;
}

/// @brief 对 new_string 中的直双引号应用弯引号转换
std::string apply_curly_double_quotes(std::string_view s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') {
            if (is_opening_context(s, i)) {
                result += kSmartDoubleLeft;  // "
            } else {
                result += kSmartDoubleRight;  // "
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

/// @brief 对 new_string 中的直单引号应用弯引号转换
std::string apply_curly_single_quotes(std::string_view s) {
    std::string result;
    result.reserve(s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\'') {
            if (is_apostrophe_context(s, i)) {
                // 缩写形式：始终右单引号 U+2019
                result += kSmartSingleRight;  // '
            } else if (is_opening_context(s, i)) {
                result += kSmartSingleLeft;  // '
            } else {
                result += kSmartSingleRight;  // '
            }
        } else {
            result += s[i];
        }
    }
    return result;
}

/// @brief 检查字符串是否含弯双引号
bool has_curly_double_quotes(std::string_view s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] == '\xE2' && s[i + 1] == '\x80') {
            char c = s[i + 2];
            if (c == '\x9C' || c == '\x9D') return true;
        }
    }
    return false;
}

/// @brief 检查字符串是否含弯单引号
bool has_curly_single_quotes(std::string_view s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] == '\xE2' && s[i + 1] == '\x80') {
            char c = s[i + 2];
            if (c == '\x98' || c == '\x99') return true;
        }
    }
    return false;
}

} // anonymous namespace

std::string normalize_quotes(std::string_view s) {
    std::string result;
    result.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + 2 < s.size() && s[i] == '\xE2' && s[i + 1] == '\x80') {
            char c = s[i + 2];
            if (c == '\x98' || c == '\x99') {
                result += '\'';
                i += 3;
                continue;
            }
            if (c == '\x9C' || c == '\x9D') {
                result += '"';
                i += 3;
                continue;
            }
        }
        result += s[i];
        ++i;
    }
    return result;
}

std::optional<std::string> find_actual_string(
    std::string_view file_content,
    std::string_view search_string
) {
    // 1. 精确匹配优先
    if (file_content.find(search_string) != std::string::npos) {
        return std::string(search_string);
    }

    // 2. 规范化两侧后查找
    auto file_norm = normalize_with_offsets(file_content);
    std::string search_norm = normalize_quotes(search_string);

    size_t pos = file_norm.normalized.find(search_norm);
    if (pos == std::string::npos) {
        return std::nullopt;
    }

    // 3. 映射回原始字节范围
    size_t orig_start = file_norm.offsets[pos];
    size_t orig_end = file_norm.offsets[pos + search_norm.size()];
    return std::string(file_content.substr(orig_start, orig_end - orig_start));
}

size_t count_actual_occurrences(
    std::string_view file_content,
    std::string_view search_string
) {
    auto actual = find_actual_string(file_content, search_string);
    if (!actual.has_value()) return 0;

    // 在原始文件内容上统计实际子串的非重叠出现次数
    size_t count = 0;
    size_t pos = 0;
    while ((pos = file_content.find(*actual, pos)) != std::string::npos) {
        ++count;
        pos += actual->size();
    }
    return count;
}

std::string preserve_quote_style(
    std::string_view old_string,
    std::string_view actual_old_string,
    std::string_view new_string
) {
    // 1. 未发生规范化 → 原样返回
    if (old_string == actual_old_string) {
        return std::string(new_string);
    }

    // 2. 检测 actual_old_string 中的弯引号家族
    bool has_double = has_curly_double_quotes(actual_old_string);
    bool has_single = has_curly_single_quotes(actual_old_string);

    // 3. 无弯引号 → 原样返回
    if (!has_double && !has_single) {
        return std::string(new_string);
    }

    // 4. 按家族应用弯引号转换
    std::string result(new_string);
    if (has_double) {
        result = apply_curly_double_quotes(result);
    }
    if (has_single) {
        result = apply_curly_single_quotes(result);
    }
    return result;
}

} // namespace agent::tool
