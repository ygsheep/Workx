/**
 * @file file_write_tool.h
 * @brief FileWriteTool — 文件写入工具
 * @details 创建或覆盖文件内容，自动创建父目录，更新时生成行级 diff。
 *          路径解析基于 ToolContext::cwd，相对路径会被自动规范化为绝对路径。
 *          v2.1.0：Phase 2 安全性增强 —— pre-read 强制检查、staleness 检测、
 *                  .bak 备份、写后状态刷新。
 * @author workx
 * @version 2.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <filesystem>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief FileWriteTool — 文件写入工具
///
/// 创建新文件或覆盖现有文件内容：
/// - 自动创建父目录（mkdir -p 语义）
/// - 更新时生成行级 diff（LCS 算法，见 diff.h）
/// - 路径解析基于 ToolContext::cwd
/// - 直接写入 content，不重写行尾（与 Claude Code 一致）
///
/// @par Phase 2 安全性增强
/// - **Pre-read 强制检查**：现有文件必须先 Read 全部，否则拒绝写入
/// - **Staleness 检测**：文件被外部修改（mtime 变化）时拒绝写入；
///   完整读取下若内容未变则放行（避免云同步/杀毒等误判）
/// - **.bak 备份**：覆盖前保存 `<path>.bak`，失败则中止写入
/// - **写后状态刷新**：写入成功后更新 FileReadStateTracker，允许连续写入
///
/// @par 执行管道
/// 1. 解析输入 → 2. 路径解析（weakly_canonical）
/// → 3. 创建父目录（create_directories）
/// → 4. 判断 create/update（fs::exists）
/// → 5. 安全检查（update 模式）：
///    ├─ Pre-read 检查（必须存在 FileReadState 且非 partial_view）
///    ├─ Staleness 检查（mtime 对比 + 内容对比回退）
///    └─ .bak 备份
/// → 6. 读取旧内容（update 模式）
/// → 7. 写入文件（std::ofstream binary）
/// → 8. 刷新 FileReadStateTracker
/// → 9. 生成 diff（update 模式） → 10. 返回结果
///
/// @par 错误处理
/// 所有错误通过 ToolResult::error() 返回，不抛异常。
/// 所有 filesystem 操作使用 std::error_code 重载。
///
/// @par 与 Claude Code 对齐
/// - description / prompt 文案对齐 CC `getWriteToolDescription()`
/// - input_schema 添加 `additionalProperties: false`（对齐 `z.strictObject`）
/// - Pre-read / Staleness 检查对齐 CC `validateInput` + `call` 的 staleness 回退
class FileWriteTool : public ITool {
   public:
    /// @brief 获取工具名称
    /// @return 工具名称常量引用 "Write"
    const std::string& name() const override;

    /// @brief 获取工具简短描述
    /// @return 描述字符串常量引用（对齐 CC）
    const std::string& description() const override;

    /// @brief 获取 LLM 提示词
    /// @return 提示词字符串常量引用，包含完整 Usage 说明（对齐 CC）
    const std::string& prompt() const override;

    /// @brief 获取输入 JSON Schema
    /// @return JSON Schema 对象，定义 file_path / content 字段，
    ///         additionalProperties: false
    nlohmann::json input_schema() const override;

    /// @brief 验证输入参数
    /// @param input 输入 JSON 对象
    /// @param ctx 工具执行上下文（当前未使用）
    /// @return 验证通过返回 ValidationResult::ok()；
    ///         file_path 缺失/非字符串/空、content 缺失/非字符串时返回错误
    ValidationResult validate_input(const nlohmann::json& input,
                                    const ToolContext& ctx) const override;

    /// @brief 权限检查（#34/#36：路径边界 + 敏感路径拦截 + Plan/Bypass 模式）
    /// @details Bypass 放行；Plan 模式禁止写入；其余按 validate_path_access 校验。
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    /// @brief 执行文件写入
    /// @param input 输入 JSON 对象（符合 input_schema）
    /// @param ctx 工具执行上下文（用于获取 cwd 解析相对路径）
    /// @return 工具结果：
    ///         - 创建成功："File created successfully at: <path>"
    ///         - 更新成功："File <path> has been updated.\n" + diff 文本
    ///         - 失败：ResultV2::err（含 pre-read / staleness / 备份失败等）
    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    /// @brief Pre-read 强制检查 + Staleness 检测
    /// @details 对齐 CC `validateInput`：
    /// - 文件存在但无读取状态 → 拒绝（"File has not been read yet..."）
    /// - 状态为 partial_view → 拒绝（需完整读取）
    /// - 当前 mtime > 状态 mtime → 拒绝（"File has been modified since read..."）
    /// - 完整读取下 mtime 变化但内容相同 → 放行（云同步/杀毒误判防护）
    /// @param canonical_path 规范化路径（FileReadStateTracker key）
    /// @param file_path 文件系统路径（用于 stat）
    /// @return 通过返回 ok，否则返回错误信息
    static ValidationResult check_pre_read_and_staleness(const std::string& canonical_path,
                                                         const std::filesystem::path& file_path);

    /// @brief 创建 .bak 备份文件
    /// @details 在同目录下创建 `<file_path>.bak`，覆盖已存在的备份。
    ///          备份失败将中止写入（安全优先）。
    /// @param file_path 待备份文件路径
    /// @return 成功返回 ok，失败返回错误信息
    static ValidationResult create_backup(const std::filesystem::path& file_path);
};

}  // namespace agent::tool
