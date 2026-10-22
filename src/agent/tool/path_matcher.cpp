/**
 * @file path_matcher.cpp
 * @brief 路径 glob 匹配工具实现
 * @details 递归下降匹配，参考 gitignore 语义简化版。
 *          所有 filesystem / 环境操作使用 error_code / getenv 容错。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/path_matcher.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

namespace agent::tool {

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr bool kCaseSensitive = false;
#else
constexpr bool kCaseSensitive = true;
#endif

/// @brief 字符相等比较（按平台大小写敏感性）
bool char_equal(char a, char b) {
    if (kCaseSensitive) return a == b;
    return std::tolower(static_cast<unsigned char>(a)) ==
           std::tolower(static_cast<unsigned char>(b));
}

/// @brief 递归匹配核心
/// @param path 路径当前位置指针
/// @param path_end 路径末尾
/// @param pattern 模式当前位置指针
/// @param pattern_end 模式末尾
/// @return 完整匹配返回 true
bool match_impl(std::string_view::const_iterator path, std::string_view::const_iterator path_end,
                std::string_view::const_iterator pattern,
                std::string_view::const_iterator pattern_end) {
    while (pattern != pattern_end) {
        // ** — 跨段通配
        if (pattern + 1 != pattern_end && *pattern == '*' && *(pattern + 1) == '*') {
            pattern += 2;
            // 跳过后续的 `/`（`**/` 形式允许跨目录）
            if (pattern != pattern_end && *pattern == '/') {
                ++pattern;
                // `**/` 后必须还有内容，否则视为匹配剩余所有
                if (pattern == pattern_end) return true;
            }

            // 尝试在 path 每个位置匹配剩余 pattern
            while (true) {
                if (match_impl(path, path_end, pattern, pattern_end)) {
                    return true;
                }
                if (path == path_end) return false;
                ++path;
            }
        }

        // * — 单段通配（不跨 /）
        if (*pattern == '*') {
            ++pattern;
            while (true) {
                if (match_impl(path, path_end, pattern, pattern_end)) {
                    return true;
                }
                if (path == path_end || *path == '/') return false;
                ++path;
            }
        }

        // ? — 单字符通配（不跨 /）
        if (*pattern == '?') {
            if (path == path_end || *path == '/') return false;
            ++pattern;
            ++path;
            continue;
        }

        // 字面量
        if (path == path_end) return false;
        if (!char_equal(*pattern, *path)) return false;
        ++pattern;
        ++path;
    }

    // 模式耗尽，path 也应耗尽
    return path == path_end;
}

}  // anonymous namespace

bool match_path_glob(std::string_view path, std::string_view pattern) {
    // 空模式只匹配空路径
    if (pattern.empty()) return path.empty();

    // 形如 `**/X` 的模式：额外尝试匹配 basename
    // （gitignore 语义：`**/foo` 等价于「任意深度下的 foo」）
    // 已由 match_impl 中的 `**` 分支自动处理

    return match_impl(path.cbegin(), path.cend(), pattern.cbegin(), pattern.cend());
}

bool matches_any_pattern(std::string_view path, const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(),
                       [&](const std::string& p) { return match_path_glob(path, p); });
}

std::string expand_home(std::string_view pattern) {
    if (pattern.empty() || pattern[0] != '~') {
        return std::string(pattern);
    }
    // 仅 ~/ 或单独 ~ 触发展开
    if (pattern.size() == 1 || pattern[1] == '/') {
        // 优先 USERPROFILE（Windows）、HOME（POSIX）
        std::string home;
#ifdef _WIN32
        if (const char* up = std::getenv("USERPROFILE")) {
            home = up;
        } else if (const char* ad = std::getenv("APPDATA")) {
            home = ad;
        }
#else
        if (const char* h = std::getenv("HOME")) {
            home = h;
        }
#endif
        if (home.empty()) {
            // 无法获取家目录，原样返回
            return std::string(pattern);
        }
        // 拼接：home + pattern[1:]
        std::string result = home;
        result.append(pattern.substr(1));
        return result;
    }
    return std::string(pattern);
}

std::string to_posix_path(std::string_view path) {
    std::string result(path);
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

}  // namespace agent::tool
