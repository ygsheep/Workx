/**
 * @file sandbox_detector.cpp
 * @brief SandboxDetector 实现
 * @details 平台条件编译探测沙盒工具：
 *          - macOS: 检查 /usr/bin/sandbox-exec 是否存在
 *          - Linux: 在 PATH 中搜索 bwrap
 *          - Windows: 直接返回 None
 *
 *          路径搜索复用 tool_registry 的 find_in_path 逻辑（避免循环依赖，
 *          此处内联简单实现）。
 *
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/process/sandbox/sandbox_detector.h"

#include <filesystem>

#ifndef _WIN32
#include <cstdlib>
#include <unistd.h>
#endif

namespace agent::process::sandbox {

namespace fs = std::filesystem;

namespace {

/// 检查文件是否存在且可执行
bool is_executable_file(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;
#ifdef _WIN32
    // Windows: 只要文件存在即可（.exe 扩展名由调用方保证）
    return true;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

/// 在 PATH 中搜索命令（POSIX 专用）
std::optional<std::string> find_in_path(const std::string& cmd) {
#ifndef _WIN32
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;

    std::string path_str = path_env;
    std::string::size_type start = 0;
    while (start <= path_str.size()) {
        auto end = path_str.find(':', start);
        if (end == std::string::npos) end = path_str.size();
        auto dir = path_str.substr(start, end - start);
        if (!dir.empty()) {
            std::string full = dir + "/" + cmd;
            if (is_executable_file(full)) return full;
        }
        start = end + 1;
    }
#else
    (void)cmd;  // Windows 下不支持 PATH 搜索
#endif
    return std::nullopt;
}

}  // namespace

SandboxDetector& SandboxDetector::instance() {
    static SandboxDetector inst;
    return inst;
}

SandboxDetector::Backend SandboxDetector::detect() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_detected) {
        m_backend = do_detect();
        m_detected = true;
    }
    return m_backend;
}

SandboxDetector::Backend SandboxDetector::do_detect() {
    m_path.reset();

#if defined(__APPLE__)
    // macOS: sandbox-exec 系统自带，固定在 /usr/bin/sandbox-exec
    const char* kSeatbeltPath = "/usr/bin/sandbox-exec";
    if (is_executable_file(kSeatbeltPath)) {
        m_path = kSeatbeltPath;
        return Backend::Seatbelt;
    }
    return Backend::None;

#elif defined(__linux__)
    // Linux: bwrap 通常在 /usr/bin/bwrap，回退到 PATH 搜索
    const char* kBwrapPath = "/usr/bin/bwrap";
    if (is_executable_file(kBwrapPath)) {
        m_path = kBwrapPath;
        return Backend::Bubblewrap;
    }
    if (auto p = find_in_path("bwrap")) {
        m_path = std::move(p);
        return Backend::Bubblewrap;
    }
    return Backend::None;

#else
    // Windows 及其他平台：无进程级沙盒后端
    return Backend::None;
#endif
}

std::optional<std::string> SandboxDetector::path() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_detected) {
        m_backend = do_detect();
        m_detected = true;
    }
    return m_path;
}

bool SandboxDetector::is_available() { return this->detect() != Backend::None; }

std::string SandboxDetector::backend_name() {
    auto b = this->detect();
    switch (b) {
        case Backend::Seatbelt:
            return "seatbelt";
        case Backend::Bubblewrap:
            return "bubblewrap";
        default:
            return "none";
    }
}

void SandboxDetector::clear_cache() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_backend = Backend::None;
    m_path.reset();
    m_detected = false;
}

}  // namespace agent::process::sandbox
