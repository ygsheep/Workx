/**
 * @file path_matcher.h
 * @brief 路径 glob 匹配工具
 * @details 简化版 gitignore 风格的路径模式匹配，用于工具系统的 deny/allow 规则。
 *          支持 * / ** / ? 通配符，不引入第三方依赖。
 *          v1.0.0：MVP 实现，覆盖 90% 用例（不支持字符类 [abc]、不支持取反 !pattern）。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agent::tool {

/// @brief 匹配单条 glob 模式
/// @details 模式语义（参考 gitignore）：
///          - `*` 匹配单段内任意字符（不含路径分隔符 `/`）
///          - `**` 跨段匹配任意数量路径组件（含零个）
///          - `?` 匹配单个非 `/` 字符
///          - 其他字符按字面量匹配
///
///          路径与模式都应先规范化为 POSIX 风格（`/` 分隔）。
///          Windows 下匹配大小写不敏感，其他平台大小写敏感。
///
/// @param path 规范化的 POSIX 路径（如 "/c/Users/foo/bar" 或 "/home/foo/bar"）
/// @param pattern glob 模式（已 ~ 展开，POSIX 风格）
/// @return 匹配返回 true
bool match_path_glob(std::string_view path, std::string_view pattern);

/// @brief 检查路径是否匹配任意模式
/// @param path 规范化路径（POSIX 风格）
/// @param patterns glob 模式列表（已 ~ 展开，POSIX 风格）
/// @return 任一模式命中返回 true
bool matches_any_pattern(std::string_view path, const std::vector<std::string>& patterns);

/// @brief 展开 `~` 为家目录
/// @details 仅展开开头的 `~/` 或 `~`（单独）。后置 `~` 不处理。
///          家目录通过 std::getenv("HOME")（POSIX）或 USERPROFILE/APPDATA（Windows）获取。
/// @param pattern 可能含 ~ 的模式
/// @return 展开后的模式（无 `~` 则原样返回）
std::string expand_home(std::string_view pattern);

/// @brief 规范化路径为 POSIX 风格
/// @details 将 `\` 替换为 `/`，不解析 `.` / `..`（调用方应已 canonical）。
///          用于把 fs::weakly_canonical 的结果转成匹配器输入格式。
/// @param path 任意风格路径
/// @return POSIX 风格路径
std::string to_posix_path(std::string_view path);

}  // namespace agent::tool
