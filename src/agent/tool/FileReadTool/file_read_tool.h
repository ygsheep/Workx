/**
 * @file file_read_tool.h
 * @brief FileReadTool — 文件读取工具
 * @details 读取文本文件内容，支持行号、偏移量、行数限制、编码检测与转换、目录列举。
 *          路径解析基于 ToolContext::cwd，相对路径会被自动规范化为绝对路径。
 * @author workx
 * @version 1.3.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <filesystem>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief FileReadTool — 文件读取工具
///
/// 支持读取文本文件：
/// - 行号显示（右对齐 + → 箭头）
/// - offset/limit 分段读取
/// - 文件编码自动检测（UTF-8/UTF-16/GBK/ASCII）
/// - 二进制文件检测（含 null 字节则拒绝读取）
/// - 目录路径自动列举内容（按字典序排序）
///
/// @par 执行管道
/// 1. 解析输入 → 2. 路径解析 → 3. 存在性检查 → 4. 目录处理
/// → 5. 常规文件检查 → 6. 文件大小检查 → 7. 编码检测
/// → 8. 流式读取（UTF-8）/全量转换（非 UTF-8） → 9. offset/limit 切片
/// → 10. 行号格式化 → 11. 附加元信息
///
/// @par 错误处理
/// 所有错误通过 ToolResult::error() 返回，不抛异常。
class FileReadTool : public ITool {
   public:
    /// @brief 获取工具名称
    /// @return 工具名称常量引用 "Read"
    const std::string& name() const override;

    /// @brief 获取工具简短描述
    /// @return 描述字符串常量引用
    const std::string& description() const override;

    /// @brief 获取 LLM 提示词
    /// @return 提示词字符串常量引用，描述工具能力与使用方式
    const std::string& prompt() const override;

    /// @brief 获取输入 JSON Schema
    /// @return JSON Schema 对象，定义 file_path/offset/limit/pages 字段
    nlohmann::json input_schema() const override;

    /// @brief 只读工具
    /// @return true（读取无副作用）
    bool is_read_only() const override { return true; }

    /// @brief 验证输入参数
    /// @param input 输入 JSON 对象
    /// @param ctx 工具执行上下文（当前未使用）
    /// @return 验证通过返回 ValidationResult::ok()，否则返回错误信息
    ValidationResult validate_input(const nlohmann::json& input,
                                    const ToolContext& ctx) const override;

    /// @brief 权限检查（#34/#36：路径边界 + 敏感文件拦截）
    /// @param input 输入 JSON 对象
    /// @param ctx 工具执行上下文（cwd 作为路径边界基准）
    /// @return 允许返回 ok；越界/敏感路径返回 PermissionDenied
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    /// @brief 执行文件读取
    /// @param input 输入 JSON 对象（符合 input_schema）
    /// @param ctx 工具执行上下文（用于获取 cwd 解析相对路径）
    /// @return 工具结果：成功返回带行号的文本，失败返回错误信息
    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    /// @brief 将文本行格式化为带行号的输出
    /// @details 每行格式：`右对齐行号→内容`，行号与内容间使用
    ///          Unicode 箭头 → (U+2192, UTF-8: \xe2\x86\x92)。
    ///          行号宽度按最大行号自动计算，末尾换行符会被移除。
    /// @param lines 文本行列表
    /// @param start_line 起始行号（1-based）
    /// @return 格式化后的字符串；空列表返回空字符串
    static std::string format_with_line_numbers(const std::vector<std::string>& lines,
                                                int start_line);

    /// @brief 列举目录内容
    /// @details 使用 directory_iterator 配合 skip_permission_denied 选项遍历，
    ///          遇到错误时跳过当前项不中断。结果按字典序排序，
    ///          目录名追加 '/' 后缀以便区分。
    /// @param dir_path 目录路径
    /// @return 工具结果：成功返回目录内容列表，空目录返回 "(empty directory)"
    static ToolResult read_directory(const std::filesystem::path& dir_path);
};

}  // namespace agent::tool
