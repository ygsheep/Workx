/**
 * @file sandbox_detector.h
 * @brief SandboxDetector — 平台沙盒工具探测
 * @details 探测当前系统可用的沙盒后端：
 *          - macOS:   `sandbox-exec`（系统自带，路径 /usr/bin/sandbox-exec）
 *          - Linux:   `bwrap`（需安装，通常在 /usr/bin/bwrap 或 /usr/local/bin/bwrap）
 *          - Windows: 无进程级沙盒后端（未来可扩展 AppContainer）
 *
 *          探测结果缓存：首次调用 detect() 后缓存 Backend 和路径，
 *          后续调用直接返回缓存值，避免每次 wrap_command() 都搜索 PATH。
 *
 *          线程安全：内部使用 std::mutex 保护缓存，可多线程调用。
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <mutex>
#include <optional>
#include <string>

namespace agent::process::sandbox {

/// @brief 平台沙盒后端探测器
/// @details 单例模式，探测并缓存当前系统可用的沙盒后端
class SandboxDetector {
   public:
    /// @brief 沙盒后端类型
    enum class Backend {
        None,        ///< 无可用后端（Windows 或工具未安装）
        Seatbelt,    ///< macOS sandbox-exec（Seatbelt）
        Bubblewrap,  ///< Linux bwrap（Bubblewrap）
    };

    /// @brief 获取单例
    static SandboxDetector& instance();

    /// @brief 探测当前系统的沙盒后端
    /// @details 首次调用执行实际探测（搜索 PATH），后续返回缓存
    /// @return 后端类型（None 表示无可用沙盒）
    Backend detect();

    /// @brief 获取沙盒工具路径
    /// @return 工具绝对路径（如 /usr/bin/sandbox-exec），无则 nullopt
    std::optional<std::string> path();

    /// @brief 沙盒是否可用
    /// @details 等价于 detect() != Backend::None
    bool is_available();

    /// @brief 后端名称（用于日志和 WrappedCommand.backend_name）
    /// @return "seatbelt" / "bubblewrap" / "none"
    std::string backend_name();

    /// @brief 清除缓存（主要用于测试）
    void clear_cache();

   private:
    SandboxDetector() = default;

    /// 执行实际探测（平台条件编译）
    Backend do_detect();

    mutable std::mutex m_mutex;
    Backend m_backend = Backend::None;
    std::optional<std::string> m_path;
    bool m_detected = false;  ///< 是否已探测过
};

}  // namespace agent::process::sandbox
