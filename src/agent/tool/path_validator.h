/**
 * @file path_validator.h
 * @brief 统一路径安全校验（#34：CWD 边界 + 敏感拦截 + Windows 可疑模式）
 * @details 纯函数无状态，供所有文件工具（Read/Write/Edit/Glob/Grep）复用。
 *          校验失败返回 Error::Code::PermissionDenied，message 含具体原因。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "core/utils/result_v2.h"

namespace agent::tool {

/// @brief 校验路径访问是否安全
/// @param path 绝对路径（应先用 expand_path 展开 ~ 与相对路径）
/// @param cwd 项目根目录（路径边界基准）
/// @param allowlist 额外允许的根前缀（可选，如用户指定的附加工作区）
/// @return ok 或 PermissionDenied（message 含具体拦截原因）
/// @details 检查顺序：
///          1. Windows 可疑模式（UNC / 8.3 短名 / ADS / \\?\ 长路径前缀 / 尾点空格）
///          2. 敏感文件/目录硬编码拦截（.env、.ssh、.git/hooks、credentials 等）
///          3. CWD 边界：存在时 canonical（解析 symlink）双重检查，
///             不存在时 weakly_canonical 检查，均须落在 cwd 或 allowlist 内
ResultV2<void> validate_path_access(std::string_view path, std::string_view cwd,
                                    const std::vector<std::string>& allowlist = {});

/// @brief Windows 可疑路径模式检测（UNC/8.3 短名/ADS/长路径前缀/尾点空格）
/// @details 跨平台纯函数（非 Windows 平台同样按 Windows 规则检测，
///          防止路径被带住 Windows 后绕过）。
bool has_suspicious_windows_pattern(std::string_view path);

/// @brief 敏感路径/文件名拦截（.env、.ssh、credentials 等硬编码清单）
bool matches_sensitive_path(std::string_view path);

/// @brief 是否绝对禁止访问（私钥/凭据等），不可通过用户确认放行（评审 #2）
/// @details 与 matches_sensitive_path 不同：绝对禁止类（如 .ssh 私钥、.git-credentials、
///          passwd/shadow）不允许用户确认放行；.env、配置文件等仅"敏感"但可由用户确认。
bool is_absolutely_forbidden_path(std::string_view path);

/// @brief 路径是否位于 cwd 或 allowlist 边界内
/// @param path 绝对路径（应已规范化）
/// @details 前缀比较，path == root 或 path 以 root + 分隔符开头均算边界内。
bool is_within_allowed_root(std::string_view path, std::string_view cwd,
                            const std::vector<std::string>& allowlist = {});

/// @brief 构造额外允许根列表（git 仓库根目录）
/// @details cwd 已由 validate_path_access 默认包含；此处仅补充 git 仓库根，
///          使「当前项目」（仓库根）及其子目录/子项目均可访问，
///          即使 cwd 位于仓库深层子目录（如 build/bin/Debug）。
inline std::vector<std::string> repo_root_allowlist(std::string_view git_repo_root) {
    std::vector<std::string> roots;
    if (!git_repo_root.empty()) roots.emplace_back(git_repo_root);
    return roots;
}

}  // namespace agent::tool