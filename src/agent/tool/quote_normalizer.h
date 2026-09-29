/**
 * @file quote_normalizer.h
 * @brief 引号规范化工具（对齐 CC findActualString + preserveQuoteStyle）
 * @details 处理 LLM 提供的 old_string 与文件实际内容的引号差异：
 *          - 弯引号 → 直引号（4 种变换，仅 U+2018/2019/201C/201D）
 *          - find_actual_string: 在文件中查找匹配，返回文件实际子串
 *          - preserve_quote_style: 写回时根据原文件引号风格还原弯引号
 *
 *          CC 只做 4 种引号变换，不做 em dash / en dash / 省略号规范化。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace agent::tool {

/// @brief 将弯引号规范化为直引号（4 种变换）
/// @details U+2018/U+2019 → ' (0x27)
///          U+201C/U+201D → " (0x22)
///          其他字符原样保留（含 em dash / en dash 等不处理）
/// @param s 原始 UTF-8 字符串
/// @return 规范化后的字符串（仅含直引号）
std::string normalize_quotes(std::string_view s);

/// @brief 在文件内容中查找匹配字符串（支持引号规范化）
/// @details 流程（对齐 CC utils.ts#findActualString）：
///          1. 精确匹配 → 返回 search_string 本身
///          2. 两侧规范化引号后查找
///          3. 命中则返回 **文件中的实际子串**（保留原引号风格）
///          4. 未命中返回 std::nullopt
/// @param file_content 文件内容
/// @param search_string 待查找的字符串（可能含直引号）
/// @return 文件中的实际子串；未找到返回 nullopt
std::optional<std::string> find_actual_string(std::string_view file_content,
                                              std::string_view search_string);

/// @brief 统计文件内容中匹配字符串的出现次数（支持引号规范化）
/// @details 使用 find_actual_string 获取实际子串后统计非重叠出现次数。
///          若 find_actual_string 未命中返回 0。
/// @param file_content 文件内容
/// @param search_string 待统计的字符串
/// @return 非重叠出现次数
size_t count_actual_occurrences(std::string_view file_content, std::string_view search_string);

/// @brief 保留原文件引号风格，将 new_string 中的直引号转换为弯引号
/// @details 流程（对齐 CC utils.ts#preserveQuoteStyle）：
///          1. old_string == actual_old_string → 未发生规范化，原样返回
///          2. 检测 actual_old_string 中的弯引号家族（单/双）
///          3. 无弯引号 → 原样返回
///          4. 按家族对 new_string 应用弯引号转换
///
///          左右方向判定（isOpeningContext）：
///          - 位于串首或前一字符为 space/tab/\\n/\\r/(/[/{/em dash/en dash → 左引号
///          - 否则 → 右引号
///          单引号特殊：两侧均为字母（缩写形式 apostrophe）→ 始终右单引号 U+2019
/// @param old_string LLM 提供的原 old_string（可能含直引号）
/// @param actual_old_string 文件中的实际子串（可能含弯引号）
/// @param new_string LLM 提供的 new_string（含直引号）
/// @return 风格保留后的 new_string（按原文件风格还原弯引号）
std::string preserve_quote_style(std::string_view old_string, std::string_view actual_old_string,
                                 std::string_view new_string);

}  // namespace agent::tool
