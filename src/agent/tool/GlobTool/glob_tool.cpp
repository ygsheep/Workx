/**
 * @file glob_tool.cpp
 * @brief GlobTool 实现
 * @details 文件名匹配工具的具体实现：glob → regex 转换 + 递归遍历 + 按修改时间排序
 * @version 1.1.0
 * @date 2026-07
 */

#include "agent/tool/GlobTool/glob_tool.h"

#include "agent/tool/path_expand.h"     // expand_path
#include "agent/tool/path_validator.h"  // validate_path_access
#include "agent/tool/permission_ask.h"  // is_bypass_mode, ask_user_confirm
#include "core/utils/error.h"

#include <format>
#include <filesystem>
#include <algorithm>
#include <sstream>
#include <vector>

namespace agent::tool {

namespace fs = std::filesystem;

// ============================================================
// 元数据方法
// ============================================================

const std::string& GlobTool::name() const {
    static const std::string n{"Glob"};
    return n;
}

const std::string& GlobTool::description() const {
    static const std::string d{"Finds files matching a glob pattern."};
    return d;
}

const std::string& GlobTool::prompt() const {
    static const std::string p{
        "Finds files matching a glob pattern. "
        "Supports ** (recursive), * (single level), and ? (single char) wildcards. "
        "Returns matching file paths sorted by modification time (newest first)."};
    return p;
}

nlohmann::json GlobTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"pattern",
               {{"type", "string"}, {"description", "The glob pattern to match files against"}}},
              {"cwd",
               {{"type", "string"},
                {"description", "The directory to search in (defaults to context cwd)"}}}}},
            {"required", {"pattern"}},
            {"additionalProperties", false}};
}

// ============================================================
// 输入验证
// ============================================================

ValidationResult GlobTool::validate_input(const nlohmann::json& input, const ToolContext& /*ctx*/
) const {
    if (!input.contains("pattern") || !input["pattern"].is_string()) {
        return ValidationResult::err(Error::Code::MissingArgument,
                                     "Missing required field: pattern");
    }
    if (input["pattern"].get<std::string>().empty()) {
        return ValidationResult::err(Error::Code::InvalidInput, "pattern must not be empty");
    }
    return ValidationResult::ok();
}

// ============================================================
// 私有辅助方法
// ============================================================

std::string GlobTool::normalize_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

/// @internal
/// @brief 递归下降 glob 匹配核心
/// @details 经典的 `*`/`**` 通配匹配算法（参考 rsync wildmatch.c 思路）：
///          - `**`  匹配任意层级（含 `/`），尝试贪婪/非贪婪两种路径
///          - `*`   匹配单层文件名（不含 `/`）
///          - `?`   匹配单个非 `/` 字符
///          - 其他字符按字面匹配
///          算法复杂度：最坏 O(P*T) 但常数极小，远优于 std::regex 编译开销。
/// @param p    模式指针
/// @param p_len 模式剩余长度
/// @param t    文本指针
/// @param t_len 文本剩余长度
/// @return true 匹配成功
static bool glob_match_impl(const char* p, size_t p_len, const char* t, size_t t_len) {
    size_t pi = 0, ti = 0;
    // star_p / star_t：记录最近一个 `*`（非 `**`）的位置，用于回溯
    size_t star_p = SIZE_MAX;
    size_t star_t = 0;
    // gstar_p：记录 `**` 跨越的位置（贪婪匹配，不回溯——因为 `**` 可吃任意字符）
    size_t gstar_p = SIZE_MAX;
    size_t gstar_t = 0;

    while (ti < t_len) {
        if (pi < p_len) {
            // `**` 匹配任意层级（含 `/`）
            if (p[pi] == '*' && pi + 1 < p_len && p[pi + 1] == '*') {
                gstar_p = pi + 2;
                gstar_t = ti;
                // 跳过 ** 后的 /（如果有）
                if (gstar_p < p_len && p[gstar_p] == '/') {
                    ++gstar_p;
                }
                pi = gstar_p;
                continue;
            }
            // `*` 匹配单层文件名（不含 `/`）
            if (p[pi] == '*') {
                star_p = pi + 1;
                star_t = ti;
                ++pi;
                continue;
            }
            // `?` 匹配单个非 `/` 字符
            if (p[pi] == '?') {
                if (t[ti] == '/') {
                    // ? 不匹配 /，回溯到 star
                    goto backtrack;
                }
                ++pi;
                ++ti;
                continue;
            }
            // 字面匹配
            if (p[pi] == t[ti]) {
                ++pi;
                ++ti;
                continue;
            }
        }

    backtrack:
        // 回溯：优先回溯单层 `*`（吃一个字符），再回溯 `**`（吃任意字符）
        if (star_p != SIZE_MAX && star_t < t_len && t[star_t] != '/') {
            // 单层 * 回溯：吃一个字符（必须非 /）
            ++star_t;
            ti = star_t;
            pi = star_p;
            continue;
        }
        if (gstar_p != SIZE_MAX) {
            // ** 回溯：吃任意字符（含 /）
            ++gstar_t;
            ti = gstar_t;
            pi = gstar_p;
            continue;
        }
        return false;
    }

    // 文本已耗尽，检查模式剩余是否全为 `*`（`*` 可匹配空，`**` 也可匹配空）
    while (pi < p_len) {
        if (p[pi] == '*') {
            ++pi;
            continue;
        }
        if (p[pi] == '*' && pi + 1 < p_len && p[pi + 1] == '*') {
            pi += 2;
            continue;
        }
        return false;
    }
    return true;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    return glob_match_impl(pattern.data(), pattern.size(), text.data(), text.size());
}

// ============================================================
// 权限检查（#60：路径边界校验，与 Read 工具一致）
// ============================================================

PermissionResult GlobTool::check_permissions(const nlohmann::json& input,
                                             const ToolContext& ctx) const {
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    // cwd 未指定时默认 ctx.cwd，已在边界内，无需校验
    const std::string raw = input.value("cwd", "");
    if (raw.empty()) return PermissionResult::ok();

    std::istringstream ss(raw);
    std::string part;
    while (std::getline(ss, part, '|')) {
        try {
            const std::string expanded = expand_path(part, ctx.cwd);
            auto res =
                validate_path_access(expanded, ctx.cwd, repo_root_allowlist(ctx.git_repo_root));
            if (res.is_err()) {
                if (!is_absolutely_forbidden_path(expanded) &&
                    ask_user_confirm(
                        ctx, std::format(
                                 "File listing access requires your approval:\n\n```\n{}\n```\n\n"
                                 "Allow listing files in this path?",
                                 part))) {
                    continue;
                }
                return PermissionResult::err(Error::Code::PermissionDenied, res.error().message);
            }
        } catch (const std::exception& e) {
            return PermissionResult::err(Error::Code::PermissionDenied,
                                         std::string("Glob path error: ") + e.what());
        }
    }
    return PermissionResult::ok();
}

// ============================================================
// 执行
// ============================================================

ResultV2<ToolResult> GlobTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 1. 解析输入（try-catch 防止类型不匹配抛异常）
    GlobInput glob_input;
    try {
        glob_input = input.get<GlobInput>();
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         std::format("Input parse failed: {}", e.what()));
    }

    // 2. 确定搜索目录
    std::string cwd = glob_input.cwd.empty() ? ctx.cwd : glob_input.cwd;
    if (cwd.empty()) {
        cwd = ".";
    }

    fs::path search_dir(cwd);
    std::error_code ec;
    if (!fs::exists(search_dir, ec)) {
        return ResultV2<ToolResult>::err(Error::Code::ResourceNotFound,
                                         "Directory does not exist: " + cwd);
    }
    if (!fs::is_directory(search_dir, ec)) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Path is not a directory: " + cwd);
    }

    // 3. 规范化 pattern（手写 glob matcher，避免 std::regex 编译开销）
    std::string normalized_pattern = normalize_path(glob_input.pattern);

    // 4. 递归遍历目录，收集匹配项
    struct MatchEntry {
        std::string relative_path;
        fs::file_time_type last_write_time;
    };

    std::vector<MatchEntry> matches;

    for (auto it = fs::recursive_directory_iterator(
             search_dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }

        const auto& entry = *it;

        // 获取相对路径并规范化
        std::string rel_path = normalize_path(fs::relative(entry.path(), search_dir, ec).string());
        if (ec) {
            ec.clear();
            continue;
        }

        // 手写 glob 匹配（O(P*T) 但常数极小，无编译开销）
        if (glob_match(normalized_pattern, rel_path)) {
            auto lwt = entry.last_write_time(ec);
            if (ec) {
                ec.clear();
                lwt = fs::file_time_type{};
            }
            matches.push_back({rel_path, lwt});
        }
    }

    // 5. 按修改时间倒序排列（最新优先）
    std::sort(matches.begin(), matches.end(), [](const MatchEntry& a, const MatchEntry& b) {
        return a.last_write_time > b.last_write_time;
    });

    // 6. 格式化输出
    if (matches.empty()) {
        return ResultV2<ToolResult>::ok(
            ToolResult::ok("No files matched pattern: " + glob_input.pattern));
    }

    std::string result;
    result.reserve(matches.size() * 40);
    for (const auto& m : matches) {
        result += m.relative_path;
        result += '\n';
    }

    // 移除末尾多余的换行
    if (!result.empty()) {
        result.pop_back();
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

}  // namespace agent::tool
