/**
 * @file shell_detector.cpp
 * @brief Shell 检测器实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/ShellTool/shell_detector.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace agent::tool::shell_detect {

#ifdef _WIN32

namespace {

/// @brief 宽字符串转 UTF-8
std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                  nullptr, nullptr);
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr,
                        nullptr);
    return out;
}

/// @brief 用 SearchPathW 在 PATH 中查找可执行文件
/// @return 找到返回绝对路径，未找到返回空字符串
std::string find_in_path(const wchar_t* name) {
    wchar_t buffer[MAX_PATH];
    DWORD len = SearchPathW(nullptr, name, L".exe", MAX_PATH, buffer, nullptr);
    if (len > 0 && len < MAX_PATH) {
        return wide_to_utf8(buffer);
    }
    return {};
}

/// @brief Windows: 查找 Git Bash
/// @details 查找顺序（优先常见安装路径，PATH 最后且排除 WSL）：
///          1. 常见系统级安装路径（C:\Program Files\Git\bin\bash.exe 等）
///          2. %LOCALAPPDATA%\Programs\Git\bin\bash.exe（用户级安装）
///          3. %GIT_INSTALL_ROOT%\bin\bash.exe（手动配置）
///          4. PATH 中的 bash.exe — 排除 C:\Windows\System32\bash.exe（WSL 启动器）
///             WSL bash 会导致路径映射问题（D:\ → /mnt/d/），不适合作为 BashTool shell
std::string find_git_bash() {
    namespace fs = std::filesystem;

    // 1. 常见系统级安装路径（最可靠，优先检查）
    const char* system_candidates[] = {
        "C:\\Program Files\\Git\\bin\\bash.exe",
        "C:\\Program Files\\Git\\usr\\bin\\bash.exe",
        "C:\\Program Files (x86)\\Git\\bin\\bash.exe",
        "C:\\Program Files (x86)\\Git\\usr\\bin\\bash.exe",
    };
    for (const char* p : system_candidates) {
        if (fs::exists(p)) return p;
    }

    // 2. %LOCALAPPDATA%\Programs\Git\bin\bash.exe（用户级安装）
    if (const char* localappdata = std::getenv("LOCALAPPDATA")) {
        fs::path user_git = fs::path(localappdata) / "Programs" / "Git" / "bin" / "bash.exe";
        if (fs::exists(user_git)) return user_git.string();
    }

    // 3. %GIT_INSTALL_ROOT%\bin\bash.exe（手动配置的环境变量）
    if (const char* git_root = std::getenv("GIT_INSTALL_ROOT")) {
        fs::path p = fs::path(git_root) / "bin" / "bash.exe";
        if (fs::exists(p)) return p.string();
    }

    // 4. PATH 中的 bash.exe — 排除 WSL 的 System32\bash.exe
    std::string path_hit = find_in_path(L"bash");
    if (!path_hit.empty() && fs::exists(path_hit)) {
        // 排除 WSL 启动器：路径包含 System32 且是 bash.exe
        std::string lower = path_hit;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower.find("system32\\bash.exe") != std::string::npos) {
            // 这是 WSL bash，跳过
        } else {
            return path_hit;
        }
    }

    return {};
}

}  // anonymous namespace

ShellInfo detect_shell_windows() {
    std::string bash = find_git_bash();
    if (!bash.empty()) {
        return {bash, "-c", ShellType::GitBash, true};
    }
    return {"cmd.exe", "/c", ShellType::CmdExe, false};
}

#endif  // _WIN32

const ShellInfo& detect() {
    static ShellInfo info = []() -> ShellInfo {
#ifdef _WIN32
        return detect_shell_windows();
#else
        return {"/bin/sh", "-c", ShellType::UnixSh, true};
#endif
    }();
    return info;
}

}  // namespace agent::tool::shell_detect
