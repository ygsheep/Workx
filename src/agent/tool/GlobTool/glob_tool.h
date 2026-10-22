/**
 * @file glob_tool.h
 * @brief GlobTool — 文件名匹配工具
 * @details 使用 glob 模式匹配文件路径，支持 **, *, ? 通配符
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief 手写 glob 匹配（递归下降，避免 std::regex 编译开销）
/// @details 支持：
///          - `*`   匹配单层文件名（不含 `/`）
///          - `**`  匹配任意层级（含 `/`）
///          - `?`   匹配单个字符（不含 `/`）
///          - 其他字符按字面匹配
/// @param pattern glob 模式
/// @param text 待匹配文本（路径已规范化为正斜杠）
/// @return true 匹配成功
/// @note #32：从 GlobTool 私有成员提升为命名空间级自由函数，供
///       mode_agent_common（Batch/Watch 的 glob 展开）与 GlobTool 复用，
///       单一实现避免语义漂移。
bool glob_match(std::string_view pattern, std::string_view text);

/// @brief GlobTool — 文件名匹配工具
///
/// 使用 glob 模式快速查找文件：
/// - 支持 **, *, ? 等通配符
/// - ** 匹配任意层级目录（含跨目录分隔符）
/// - *  匹配单层文件名（不含路径分隔符）
/// - ?  匹配单个字符
/// - 结果按修改时间倒序排列（最新优先）
class GlobTool : public ITool {
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

    /// @brief 验证输入：pattern 不能为空
    ValidationResult validate_input(const nlohmann::json& input,
                                    const ToolContext& ctx) const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

   private:
    /// @brief 规范化路径分隔符为正斜杠
    /// @param path 原始路径
    /// @return 规范化后的路径
    static std::string normalize_path(const std::string& path);
};

}  // namespace agent::tool
