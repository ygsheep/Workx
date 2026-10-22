/**
 * @file tool_registry.cpp
 * @brief ToolRegistry 实现
 * @details 外部工具路径发现：config > bundled > PATH > nullopt
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/process/tool_registry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace agent::process {

namespace fs = std::filesystem;

ToolRegistry& ToolRegistry::instance() {
    static ToolRegistry registry;
    return registry;
}

std::string ToolRegistry::executable_dir() { return get_executable_dir(); }

std::string ToolRegistry::get_executable_dir() {
#ifdef _WIN32
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    fs::path exe_path(path);
    return exe_path.parent_path().string();
#else
    char path[4096];
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return {};
    path[len] = '\0';
    fs::path exe_path(path);
    return exe_path.parent_path().string();
#endif
}

bool ToolRegistry::is_executable(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return false;
#ifdef _WIN32
    // Windows: 文件存在即可执行（扩展名决定是否可运行）
    return true;
#else
    // POSIX: 检查可执行权限
    return access(path.c_str(), X_OK) == 0;
#endif
}

std::optional<std::string> ToolRegistry::find_in_path(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env || path_env[0] == '\0') return std::nullopt;

    std::string path_str(path_env);
    size_t start = 0;
    while (start <= path_str.size()) {
        size_t end = path_str.find(
#ifdef _WIN32
            ';'
#else
            ':'
#endif
            ,
            start);
        if (end == std::string::npos) end = path_str.size();
        if (end > start) {
            std::string dir = path_str.substr(start, end - start);
            fs::path candidate = fs::path(dir) / name;
            if (is_executable(candidate.string())) {
                return fs::absolute(candidate).lexically_normal().string();
            }
        }
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> ToolRegistry::find_executable(const std::string& name) {
    if (auto found = find_in_path(name)) return found;
#ifdef _WIN32
    // Windows: PATH 中常省略 .exe 扩展名，自动补试一次
    constexpr std::string_view kExeExt = ".exe";
    if (name.size() < kExeExt.size() ||
        name.compare(name.size() - kExeExt.size(), kExeExt.size(), kExeExt) != 0) {
        return find_in_path(name + std::string(kExeExt));
    }
#endif
    return std::nullopt;
}

std::optional<std::string> ToolRegistry::resolve_tool(const std::string& tool_name,
                                                      const std::string& bundled_relative_path,
                                                      const std::string& path_name) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. 查缓存
    auto it = m_cache.find(tool_name);
    if (it != m_cache.end()) {
        return it->second;
    }

    std::optional<std::string> result;

    // 2. bundled: <exe_dir>/<bundled_relative_path>
    std::string exe_dir = get_executable_dir();
    if (!exe_dir.empty()) {
        fs::path bundled = fs::path(exe_dir) / bundled_relative_path;
        if (is_executable(bundled.string())) {
            result = fs::absolute(bundled).lexically_normal().string();
        }
    }

    // 3. PATH: which rg / where rg.exe
    if (!result) {
        result = find_in_path(path_name);
    }

    // 4. 缓存结果（包括 nullopt）
    m_cache[tool_name] = result;
    return result;
}

std::optional<std::string> ToolRegistry::resolve_ripgrep() const {
#ifdef _WIN32
    return resolve_tool("ripgrep", "tools/rg.exe", "rg.exe");
#else
    return resolve_tool("ripgrep", "tools/rg", "rg");
#endif
}

std::optional<std::string> ToolRegistry::resolve_jq() const {
#ifdef _WIN32
    return resolve_tool("jq", "tools/jq.exe", "jq.exe");
#else
    return resolve_tool("jq", "tools/jq", "jq");
#endif
}

void ToolRegistry::clear_cache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache.clear();
}

}  // namespace agent::process
