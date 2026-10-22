/**
 * @file path_expand.cpp
 * @brief 路径展开工具实现
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "path_expand.h"

#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <system_error>

namespace agent::tool {

namespace fs = std::filesystem;

namespace {

/// 获取用户 home 目录（POSIX 用 getenv("HOME"), Windows 用 USERPROFILE）
std::string get_home_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) return p;
    // 兜底：HOMEDRIVE + HOMEPATH
    if (const char* drive = std::getenv("HOMEDRIVE")) {
        if (const char* path = std::getenv("HOMEPATH")) {
            return std::string(drive) + path;
        }
    }
#else
    if (const char* p = std::getenv("HOME")) return p;
#endif
    return {};
}

}  // namespace

std::string expand_path(std::string_view path, std::string_view base_dir) {
    // 安全检查：null 字节
    if (path.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("Path contains null bytes");
    }

    // trim 空白
    std::string trimmed(path);
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t')) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t')) {
        trimmed.pop_back();
    }

    if (trimmed.empty()) {
        // 空路径：返回 base_dir（或进程 cwd）
        if (!base_dir.empty()) return std::string(base_dir);
        std::error_code ec;
        return fs::current_path(ec).string();
    }

    // ~ 展开
    if (trimmed == "~") {
        return get_home_dir();
    }
    if (trimmed.size() >= 2 && trimmed[0] == '~' && (trimmed[1] == '/' || trimmed[1] == '\\')) {
        std::string home = get_home_dir();
        if (!home.empty()) {
            return home + trimmed.substr(1);  // 保留分隔符
        }
    }

    fs::path p(trimmed);
    std::error_code ec;

    // 绝对路径：normalize 后返回
    if (p.is_absolute()) {
        return fs::absolute(p, ec).lexically_normal().string();
    }

    // 相对路径：基于 base_dir 解析
    fs::path base;
    if (!base_dir.empty()) {
        base = fs::path(base_dir);
    } else {
        base = fs::current_path(ec);
    }
    return fs::absolute(base / p, ec).lexically_normal().string();
}

}  // namespace agent::tool
