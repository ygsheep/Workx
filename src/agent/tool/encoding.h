/**
 * @file encoding.h
 * @brief 文件编码检测与转换
 * @details 支持 UTF-8/UTF-16/GBK/ASCII 编码检测，并将非 UTF-8 内容转换为 UTF-8。
 *          检测顺序：BOM → null 字节（二进制）→ UTF-8 验证 → GBK 启发式。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>

namespace agent::tool {

/// @brief 文件编码类型
enum class Encoding {
    Utf8,     ///< UTF-8（含或不含 BOM）
    Utf16LE,  ///< UTF-16 Little Endian（含 BOM）
    Utf16BE,  ///< UTF-16 Big Endian（含 BOM）
    Gbk,      ///< GBK/GB2312（中文，无 BOM）
    Ascii,    ///< ASCII（纯 7-bit，UTF-8 子集）
    Binary,   ///< 二进制文件（含 null 字节，非 UTF-16）
    Unknown,  ///< 未知编码
};

/// @brief 检测文件编码
/// @details 检测顺序：
///          1. BOM 检测（UTF-8/UTF-16 LE/BE）
///          2. null 字节检测（二进制判定，UTF-16 已由 BOM 排除）
///          3. UTF-8 多字节序列验证
///          4. GBK 双字节启发式检测
/// @param path 文件路径
/// @return 检测到的编码类型；空文件返回 Utf8
Encoding detect_encoding(const std::filesystem::path& path);

/// @brief 读取文件并转换为 UTF-8 行列表
/// @details 根据编码类型选择读取策略：
///          - UTF-8/ASCII：直接读取（跳过 BOM）
///          - UTF-16：全量读取 + 手动转换为 UTF-8
///          - GBK：全量读取 + 平台 API 转换（Windows: MultiByteToWideChar）
/// @param path 文件路径
/// @param encoding 编码类型（通常由 detect_encoding 获取）
/// @return UTF-8 文本行列表；读取失败返回空列表
std::vector<std::string> read_as_utf8_lines(const std::filesystem::path& path, Encoding encoding);

/// @brief 读取文件并转换为单个 UTF-8 字符串（保留行尾）
/// @details 与 read_as_utf8_lines 不同，本函数返回完整内容（含原始行尾）：
///          - UTF-8/ASCII：跳过 BOM，原样返回字节流
///          - UTF-16LE/BE：跳过 BOM，转换为 UTF-8（CRLF/CR 字节随之转为 UTF-8 字节）
///          - GBK：转换为 UTF-8
///          适合 FileEditTool 在 LF 规范化前获取原始 UTF-8 内容。
/// @param path 文件路径
/// @param encoding 编码类型（通常由 detect_encoding 获取）
/// @return UTF-8 文本内容；读取失败返回空字符串
std::string read_file_as_utf8(const std::filesystem::path& path, Encoding encoding);

/// @brief 将 UTF-8 内容按指定编码原子写入文件（保留原编码 + BOM）
/// @details 用于 FileEditTool 写回时保留原文件编码：
///          - UTF-8/ASCII：原样写入（不加 BOM，对齐 CC 行为）
///          - UTF-16LE：写入 FF FE BOM + UTF-16LE 编码字节
///          - UTF-16BE：写入 FE FF BOM + UTF-16BE 编码字节
///          - GBK：转换为 GBK 字节流写入
///          Binary/Unknown 回退为 UTF-8 原样写入。
///          写入先落盘同目录临时文件（<name>.workx.tmp），成功后再
///          rename 原子替换，避免中断/崩溃导致原文件半写损坏（#23 P2）。
/// @param path 文件路径
/// @param utf8_content UTF-8 文本内容
/// @param encoding 目标编码类型
/// @param is_cancelled 可选取消回调：rename 前检查，已取消则删除临时文件并返回 false
/// @return true 成功；false 失败（写入错误或已取消）
bool write_file_with_encoding(const std::filesystem::path& path, const std::string& utf8_content,
                              Encoding encoding, const std::function<bool()>& is_cancelled = {});

/// @brief 获取编码名称（用于日志/错误信息）
/// @param encoding 编码类型
/// @return 编码名称字符串（如 "UTF-8"、"GBK"）
const char* encoding_name(Encoding encoding);

/// @brief 规范化行尾：去除行末的 '\r'（CRLF → LF）
/// @details std::getline 读取 CRLF 文件时仅剥离 '\n'，行末会残留 '\r'。
///          本函数移除行末 '\r'，保证内部存储统一为 LF 风格，
///          避免输出混入 '\r' 导致 TUI 显示异常。
/// @param line 待处理的行（原地修改）
void normalize_eol(std::string& line);

/// @brief 跳过 UTF-8 BOM（若存在）
/// @details 读取 ifstream 前 3 字节检测 BOM (EF BB BF)。
///          - 若存在 BOM：流位置跳过 BOM，返回 true
///          - 若不存在：流位置重置到开头，返回 false
/// @param file 已打开的输入流（文本模式）
/// @return true 表示 BOM 已跳过；false 表示无 BOM
bool skip_utf8_bom(std::ifstream& file);

/// @brief 清洗字符串中的非法 UTF-8 字节，替换为 U+FFFD（U+FFFD: EF BF BD）
/// @details 用于将任意字节序列规整为合法 UTF-8，避免 nlohmann::json 在
///          dump()/序列化时因遇到不完整 UTF-8 序列抛出 type_error.316。
///          会保留合法多字节序列，仅替换非法/孤立续字节与截断的序列。
/// @param text 待清洗的原始字节序列
/// @return 清洗后的合法 UTF-8 字符串
std::string sanitize_utf8(std::string_view text);

/// @brief 递归清洗 nlohmann::json 中所有字符串字段的非法 UTF-8 字节
/// @details 递归遍历 object/array，对每个 string 值调用 sanitize_utf8。
///          返回新的 json（不修改入参），保证 dump() 不会抛 type_error.316。
/// @param j 待清洗的 json
/// @return 清洗后的 json（所有 string 均为合法 UTF-8）
nlohmann::json sanitize_json_strings(const nlohmann::json& j);

}  // namespace agent::tool
