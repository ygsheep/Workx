/**
 * @file file_edit_tool.h
 * @brief FileEditTool — 文件编辑工具
 * @details 通过精确字符串匹配替换文件内容：
 *          - replace_all=false: 替换第一个匹配（需唯一匹配，否则报错）
 *          - replace_all=true: 替换所有匹配
 *          v1.2.0：Phase 2 完成 —— validate_input 覆盖 CC 全部 P0/P1/P2/P3 错误码：
 *                  0 (secret 扫描，配置开关) / 2 (deny 规则，配置驱动) /
 *                  1/3/4/5/6/7/8/9/10 (Phase 1 已实现)。
 *          v1.1.0：Phase 1 实现 —— 基础替换流程、validate_input P0 检查、
 *                  pre-read 强制检查、staleness 检测、.bak 备份、写后状态刷新。
 *                  LF 规范化匹配策略（CRLF 文件行尾保留留到 Phase 3）。
 * @author workx
 * @version 1.2.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <filesystem>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief FileEditTool — 文件编辑工具
///
/// 通过精确字符串匹配替换文件内容：
/// - replace_all=false: 替换第一个匹配（old_string 必须唯一，否则报错）
/// - replace_all=true: 替换所有匹配
///
/// @par Phase 2 已实现（v1.2.0）
/// - 错误码 0: secret 扫描（ConfigManager: tool.edit.scan_secrets 开关控制）
/// - 错误码 2: deny 规则（ConfigManager: tool.edit.deny_patterns 配置驱动，
///   换行分隔的 glob 模式，支持 * / ** / ?，~ 展开）
///
/// @par Phase 1 已实现（v1.1.0）
/// - input_schema 补全 additionalProperties: false
/// - prompt 完整 Usage 文本（对齐 CC）
/// - validate_input P0 检查（old/new
/// 相等、文件大小、存在性、.ipynb、预读、staleness、匹配、唯一性）
/// - call 基础替换流程（单次/replace_all）
/// - pre-read 强制检查 + staleness 检测（对齐 FileWriteTool）
/// - .bak 备份（覆盖前）
/// - 写后 FileReadStateTracker 刷新
/// - 行级 diff 生成（复用 FileWriteTool/diff.h）
///
/// @par Phase 3 限制
/// - LF 规范化匹配：读取后 CRLF → LF，在 LF 版本上匹配替换，写回 LF。
///   CRLF 文件编辑后行尾会变为 LF。lineEndings 保留留到 Phase 3。
/// - 不做引号规范化（弯引号 → 直引号），留到 Phase 3。
/// - encoding 完整支持（UTF-16）留到 Phase 3。
///
/// @par 执行管道
/// 1. 解析输入 → FileEditInput
/// 2. 路径解析（weakly_canonical，相对路径基于 ctx.cwd）
/// 3. 判断新建/更新（fs::exists）
/// 4. 新建模式（old_string 必须为空，validate_input 已检查）：
///    ├─ 创建父目录
///    ├─ 写入 new_string
///    ├─ 刷新 FileReadStateTracker
///    └─ 返回 "File created successfully"
/// 5. 更新模式：
///    ├─ Pre-read + Staleness 检查
///    ├─ 读取文件内容（LF 规范化）
///    ├─ 匹配 old_string（统计匹配数）
///    ├─ 唯一性检查（!replace_all 且匹配数 > 1 → 拒绝）
///    ├─ 替换（单次或全量）
///    ├─ .bak 备份
///    ├─ 写入文件
///    ├─ 刷新 FileReadStateTracker
///    ├─ 生成 diff
///    └─ 返回 "File has been updated" + diff
///
/// @par 错误处理
/// 所有错误通过 ResultV2<ToolResult>::err(Error) 返回，不抛异常。
/// 所有 filesystem 操作使用 std::error_code 重载。
class FileEditTool : public ITool {
   public:
    /// @brief 获取工具名称
    /// @return 工具名称常量引用 "Edit"
    const std::string& name() const override;

    /// @brief 获取工具简短描述
    /// @return 描述字符串常量引用（对齐 CC）
    const std::string& description() const override;

    /// @brief 获取 LLM 提示词
    /// @return 提示词字符串常量引用，包含完整 Usage 说明（对齐 CC）
    const std::string& prompt() const override;

    /// @brief 获取输入 JSON Schema
    /// @return JSON Schema 对象，定义 file_path / old_string / new_string / replace_all 字段，
    ///         additionalProperties: false（对齐 z.strictObject）
    nlohmann::json input_schema() const override;

    /// @brief 验证输入参数
    /// @param input 输入 JSON 对象
    /// @param ctx 工具执行上下文（当前未使用）
    /// @return 验证通过返回 ValidationResult::ok()；
    ///         否则返回错误信息（对应 README §5.3 错误码 0-10）
    ValidationResult validate_input(const nlohmann::json& input,
                                    const ToolContext& ctx) const override;

    /// @brief 权限检查（#34/#36：路径边界 + 敏感路径拦截 + Plan/Bypass 模式）
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    /// @brief 执行文件编辑
    /// @param input 输入 JSON 对象（符合 input_schema）
    /// @param ctx 工具执行上下文（用于获取 cwd 解析相对路径）
    /// @return 工具结果：
    ///         - 新建成功："File created successfully at: <path>"
    ///         - 更新成功："The file <path> has been updated successfully.\n" + diff 文本
    ///         - 失败：ResultV2::err（含 pre-read / staleness / 匹配失败 / 备份失败等）
    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    /// @brief Pre-read 强制检查 + Staleness 检测
    /// @details 对齐 CC validateInput + FileWriteTool::check_pre_read_and_staleness：
    /// - 文件存在但无读取状态 → 拒绝（"File has not been read yet..."）
    /// - 状态为 partial_view 且 old_string 起始行不在已读范围 → 拒绝
    /// - 当前 mtime > 状态 mtime → 拒绝（"File has been modified since read..."）
    /// - 完整读取下 mtime 变化但内容相同 → 放行（云同步/杀毒误判防护）
    /// @param canonical_path 规范化路径（FileReadStateTracker key）
    /// @param file_path 文件系统路径（用于 stat）
    /// @param old_string 待替换的旧文本（用于定位编辑目标行；空串表示新建/整文件场景）
    /// @return 通过返回 ok，否则返回错误信息
    static ValidationResult check_pre_read_and_staleness(const std::string& canonical_path,
                                                         const std::filesystem::path& file_path,
                                                         const std::string& old_string);

    /// @brief 创建 .bak 备份文件
    /// @details 在同目录下创建 `<file_path>.bak`，覆盖已存在的备份。
    ///          备份失败将中止写入（安全优先）。
    /// @param file_path 待备份文件路径
    /// @return 成功返回 ok，失败返回错误信息
    static ValidationResult create_backup(const std::filesystem::path& file_path);
};

}  // namespace agent::tool
