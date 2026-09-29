/**
 * @file grep_tool.h
 * @brief GrepTool — 内容搜索工具
 * @details 调用 ripgrep (rg) 在文件中搜索匹配的行，支持正则和字面量
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief GrepTool — 内容搜索工具
///
/// 在指定路径下搜索文件内容：
/// - 内部调用捆绑的 ripgrep (rg) 实现高性能搜索
/// - 支持正则表达式和字面量匹配
/// - 支持大小写敏感/忽略
/// - 支持 glob 过滤文件（如 *.cpp）
/// - 返回匹配的文件名、行号、行内容
class GrepTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    /// @brief 只读工具（无副作用）
    bool is_read_only() const override { return true; }

    /// @brief 权限检查（#60：路径边界校验，与 Read 工具一致）
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
