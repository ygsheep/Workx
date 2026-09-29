/**
 * @file types.h
 * @brief 工具输入/输出类型定义
 * @details FileRead/Edit/Write/Glob/Grep 工具的输入输出结构体
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace agent::tool {

// ============================================================
// FileReadTool
// ============================================================

/// @brief FileRead 工具输入
struct FileReadInput {
    std::string file_path;             ///< 文件路径
    std::optional<int> offset;         ///< 起始行号（1-based，默认 1）
    std::optional<int> limit;          ///< 读取行数限制
    std::optional<std::string> pages;  ///< PDF 页码范围（如 "1-5"、"3"、"10-20"）
};

/// @brief FileRead 工具输出
struct FileReadOutput {
    std::string type;       ///< "text" | "image" | "notebook" | "pdf"
    std::string file_path;  ///< 文件路径
    std::string content;    ///< 文件内容
    int total_lines{0};     ///< 总行数
    int start_line{0};      ///< 起始行号
};

// ============================================================
// FileEditTool
// ============================================================

/// @brief FileEdit 工具输入
struct FileEditInput {
    std::string file_path;    ///< 文件路径
    std::string old_string;   ///< 待替换字符串
    std::string new_string;   ///< 替换后字符串
    bool replace_all{false};  ///< 是否替换所有匹配
};

/// @brief FileEdit 工具输出
struct FileEditOutput {
    std::string file_path;            ///< 文件路径
    std::string content;              ///< 编辑后内容
    nlohmann::json structured_patch;  ///< 结构化差异
    std::string original_file;        ///< 原始文件内容
};

// ============================================================
// FileWriteTool
// ============================================================

/// @brief FileWrite 工具输入
struct FileWriteInput {
    std::string file_path;  ///< 文件路径
    std::string content;    ///< 写入内容
};

/// @brief FileWrite 工具输出
struct FileWriteOutput {
    std::string type;                 ///< "create" | "update"
    std::string file_path;            ///< 文件路径
    std::string content;              ///< 写入内容
    nlohmann::json structured_patch;  ///< 结构化差异
    std::string original_file;        ///< 原始文件内容（更新时）
};

// ============================================================
// GlobTool
// ============================================================

/// @brief Glob 工具输入
struct GlobInput {
    std::string pattern;  ///< glob 匹配模式
    std::string cwd;      ///< 工作目录
};

/// @brief Glob 工具输出
struct GlobOutput {
    std::vector<std::string> files;        ///< 匹配的文件列表
    std::vector<std::string> directories;  ///< 匹配的目录列表
};

// ============================================================
// GrepTool
// ============================================================

/// @brief Grep 工具输入
struct GrepInput {
    std::string pattern;           ///< 搜索模式（正则或字面量）
    std::string path;              ///< 搜索路径
    bool case_insensitive{false};  ///< 是否忽略大小写
    bool regex{true};              ///< 是否为正则表达式
};

/// @brief Grep 工具输出
struct GrepOutput {
    /// @brief 单个匹配结果
    struct Match {
        std::string file_path;     ///< 文件路径
        int line_number{0};        ///< 行号
        std::string line_content;  ///< 匹配行内容
    };

    std::vector<Match> matches;  ///< 匹配结果列表
};

// ============================================================
// JSON 序列化（NLOHMANN_DEFINE_TYPE_* 宏）
// ============================================================

// 输入结构体使用 WITH_DEFAULT：LLM 可省略有默认值/可选字段
// 输出结构体使用普通版本：仅序列化（tool → JSON），不涉及反序列化
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileReadInput, file_path, offset, limit, pages)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileReadOutput, type, file_path, content, total_lines,
                                   start_line)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileEditInput, file_path, old_string, new_string,
                                                replace_all)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileEditOutput, file_path, content, structured_patch,
                                   original_file)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(FileWriteInput, file_path, content)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(FileWriteOutput, type, file_path, content, structured_patch,
                                   original_file)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GlobInput, pattern, cwd)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GlobOutput, files, directories)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(GrepInput, pattern, path, case_insensitive, regex)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GrepOutput::Match, file_path, line_number, line_content)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GrepOutput, matches)

}  // namespace agent::tool
