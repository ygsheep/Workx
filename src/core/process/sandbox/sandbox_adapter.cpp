/**
 * @file sandbox_adapter.cpp
 * @brief SandboxAdapter 实现
 * @details 平台条件编译生成沙盒 profile：
 *          - macOS: 生成 SBPL（Scheme-Based Profile Language）字符串，通过 sandbox-exec -p 传入
 *          - Linux: 生成 bwrap 命令行参数（--bind / --tmpfs / --unshare-net 等）
 *          - Windows: 无包装，返回降级结果
 *
 * @par Profile 安全性
 * 路径参数在插入 profile 前做转义：
 * - macOS SBPL: 路径中的 `"` 和 `\` 转义为 `\"` 和 `\\`，整体用 "..." 包裹
 * - Linux bwrap: 路径直接作为命令行参数（由 execvp 自动处理，无需额外转义）
 *
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/process/sandbox/sandbox_adapter.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include "core/process/sandbox/sandbox_detector.h"

namespace agent::process::sandbox {

namespace fs = std::filesystem;

namespace {

// ============================================================
// 平台无关辅助
// ============================================================

/// 获取系统临时目录路径
std::string get_temp_dir() {
#ifdef _WIN32
    if (const char* tmp = std::getenv("TEMP")) return tmp;
    if (const char* tmp = std::getenv("TMP")) return tmp;
    return "C:\\Windows\\Temp";
#else
    if (const char* tmp = std::getenv("TMPDIR")) return tmp;
    return "/tmp";
#endif
}

/// 路径规范化（去除尾部 /，解析 . 和 ..）
std::string normalize_path(const std::string& p) {
    std::error_code ec;
    auto result = fs::weakly_canonical(p, ec).string();
    if (ec) return p;  // 解析失败返回原路径
    // 去除尾部分隔符（保留根路径 "/"）
    while (result.size() > 1 && result.back() == '/') {
        result.pop_back();
    }
    return result;
}

// ============================================================
// macOS Seatbelt profile 生成
// ============================================================
#if defined(__APPLE__)

/// 转义 SBPL 字符串字面量中的特殊字符
std::string escape_sbpl_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    out += '"';
    for (char c : s) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out += c;
        }
    }
    out += '"';
    return out;
}

/// 生成 macOS Seatbelt SBPL profile
std::string generate_seatbelt_profile(const SandboxConfig& config) {
    std::ostringstream ss;

    ss << "(version 1)\n";
    ss << "(deny default)\n";  // 默认拒绝所有

    // 进程基础能力
    ss << "(allow process-fork)\n";
    ss << "(allow signal (target self))\n";
    ss << "(allow process-info* (target self))\n";
    ss << "(allow sysctl-read)\n";
    ss << "(allow file-read*\n";
    ss << "  (literal \"/dev/urandom\")\n";
    ss << "  (literal \"/dev/null\")\n";
    ss << "  (literal \"/dev/zero\")\n";
    ss << ")\n";

    // 系统目录读权限
    if (config.allow_system_read) {
        const char* sys_paths[] = {"/usr", "/lib",  "/lib64",  "/etc",
                                   "/bin", "/sbin", "/System", "/Library"};
        for (const char* p : sys_paths) {
            ss << "(allow file-read* (subpath " << escape_sbpl_string(p) << "))\n";
        }
    }

    // 临时目录写权限
    if (config.allow_temp_write) {
        std::string tmp = normalize_path(get_temp_dir());
        ss << "(allow file-write* (subpath " << escape_sbpl_string(tmp) << "))\n";
        ss << "(allow file-read* (subpath " << escape_sbpl_string(tmp) << "))\n";
    }

    // 用户配置的读权限
    for (const auto& p : config.allow_read) {
        std::string np = normalize_path(p);
        ss << "(allow file-read* (subpath " << escape_sbpl_string(np) << "))\n";
    }

    // 用户配置的写权限
    for (const auto& p : config.allow_write) {
        std::string np = normalize_path(p);
        ss << "(allow file-write* (subpath " << escape_sbpl_string(np) << "))\n";
        ss << "(allow file-read* (subpath " << escape_sbpl_string(np) << "))\n";
    }

    // deny 规则（优先级高于 allow）
    for (const auto& p : config.deny_read) {
        std::string np = normalize_path(p);
        ss << "(deny file-read* (subpath " << escape_sbpl_string(np) << "))\n";
    }
    for (const auto& p : config.deny_write) {
        std::string np = normalize_path(p);
        ss << "(deny file-write* (subpath " << escape_sbpl_string(np) << "))\n";
    }

    // 网络规则
    // 语义优先级：
    //   1. network_isolated=true → 完全拒绝网络
    //   2. allow_domains 非空 → 白名单模式（默认拒绝，仅允许 allow_domains，
    //      deny_domains 仍可从中排除特定域名，SBPL 中 deny 优先于 allow）
    //   3. allow_domains 为空、deny_domains 非空 → 黑名单模式（默认允许，拒绝 deny_domains）
    //   4. 两者都为空 → 允许所有网络
    if (config.network_isolated) {
        ss << "(deny network*)\n";
    } else if (!config.allow_domains.empty()) {
        // 白名单模式：默认拒绝，仅允许 allow_domains
        ss << "(deny network*)\n";
        for (const auto& domain : config.allow_domains) {
            ss << "(allow network* (remote tcp " << escape_sbpl_string(domain) << "))\n";
        }
        // deny_domains 在白名单模式下仍生效（从 allow 列表中排除特定域名）
        for (const auto& domain : config.deny_domains) {
            ss << "(deny network* (remote tcp " << escape_sbpl_string(domain) << "))\n";
        }
    } else if (!config.deny_domains.empty()) {
        // 黑名单模式：默认允许，拒绝 deny_domains
        ss << "(allow network*)\n";
        for (const auto& domain : config.deny_domains) {
            ss << "(deny network* (remote tcp " << escape_sbpl_string(domain) << "))\n";
        }
    } else {
        // 无限制
        ss << "(allow network*)\n";
    }

    return ss.str();
}

#endif  // __APPLE__

// ============================================================
// Linux Bubblewrap 参数生成
// ============================================================
#if defined(__linux__)

/// 生成 bwrap 命令行参数
/// @details bwrap 通过 --bind/--ro-bind/--tmpfs 等参数构建沙盒文件系统视图
std::vector<std::string> generate_bwrap_args(const SandboxConfig& config) {
    std::vector<std::string> args;

    // 根文件系统：只读 bind（子进程看到完整 / 但默认只读）
    args.push_back("--ro-bind");
    args.push_back("/");
    args.push_back("/");

    // 挂载 /dev 和 /proc（许多工具需要）
    args.push_back("--dev");
    args.push_back("/dev");
    args.push_back("--proc");
    args.push_back("/proc");

    // 临时目录：独立 tmpfs（隔离主机 /tmp）
    if (config.allow_temp_write) {
        args.push_back("--tmpfs");
        args.push_back("/tmp");
    }

    // 用户配置的可写目录：bind mount
    for (const auto& p : config.allow_write) {
        std::string np = normalize_path(p);
        args.push_back("--bind");
        args.push_back(np);
        args.push_back(np);
    }

    // 用户配置的额外只读目录：ro-bind（显式声明，即使 / 已 ro-bind 也无害）
    for (const auto& p : config.allow_read) {
        std::string np = normalize_path(p);
        args.push_back("--ro-bind");
        args.push_back(np);
        args.push_back(np);
    }

    // 网络隔离
    if (config.network_isolated) {
        args.push_back("--unshare-net");
    }

    // deny_write/deny_read：用 --tmpfs 覆盖对应路径，使其在沙盒内不可访问
    // bwrap 是命名空间隔离（非路径过滤），--tmpfs 在该挂载点创建空 tmpfs，
    // 遮蔽原路径内容。deny_write 路径用 tmpfs 覆盖（不可读写原内容）；
    // deny_read 路径同理。这是 bwrap 下最接近 deny 语义的方案。
    // 注意：若 deny 路径同时出现在 allow 中，--tmpfs 必须在 --bind 之后才能遮蔽，
    //       bwrap 按参数顺序挂载，后挂载覆盖先挂载，因此 deny 放最后。
    for (const auto& p : config.deny_write) {
        std::string np = normalize_path(p);
        args.push_back("--tmpfs");
        args.push_back(np);
    }
    for (const auto& p : config.deny_read) {
        std::string np = normalize_path(p);
        // 避免与 deny_write 重复挂载
        bool already_denied = false;
        for (const auto& dw : config.deny_write) {
            if (normalize_path(dw) == np) {
                already_denied = true;
                break;
            }
        }
        if (!already_denied) {
            args.push_back("--tmpfs");
            args.push_back(np);
        }
    }

    return args;
}

#endif  // __linux__

}  // namespace

// ============================================================
// SandboxConfig 工厂方法实现
// ============================================================

SandboxConfig SandboxConfig::restrictive(const std::string& cwd) {
    SandboxConfig config;
    std::string ncwd = normalize_path(cwd);
    if (!ncwd.empty()) {
        config.allow_write.push_back(ncwd);
        config.allow_read.push_back(ncwd);
    }
    // 系统目录读权限和临时目录写权限由默认值（true）开启
    return config;
}

SandboxConfig SandboxConfig::permissive() {
    SandboxConfig config;
    config.network_isolated = false;
    config.allow_system_read = true;
    config.allow_temp_write = true;
    // 所有 allow/deny 列表为空 → 无限制
    return config;
}

bool SandboxConfig::is_permissive() const noexcept {
    // network_isolated=false 且无任何 deny 规则且无 allow 限制
    // （allow 为空 + network_isolated=false 意味着无任何限制）
    return !network_isolated && deny_write.empty() && deny_read.empty() && deny_domains.empty() &&
           allow_write.empty() && allow_read.empty() && allow_domains.empty();
}

// ============================================================
// SandboxAdapter 实现
// ============================================================

WrappedCommand SandboxAdapter::wrap_command(const std::string& cmd,
                                            const std::vector<std::string>& args,
                                            const SandboxConfig& config) {
    // 宽松配置：直接返回原命令
    if (config.is_permissive()) {
        return make_passthrough(cmd, args);
    }

    auto& detector = SandboxDetector::instance();
    auto backend = detector.detect();

    switch (backend) {
        case SandboxDetector::Backend::Seatbelt: {
            auto path = detector.path();
            if (path) {
                return wrap_with_seatbelt(*path, cmd, args, config);
            }
            return make_degraded(cmd, args, "none");
        }
        case SandboxDetector::Backend::Bubblewrap: {
            auto path = detector.path();
            if (path) {
                return wrap_with_bubblewrap(*path, cmd, args, config);
            }
            return make_degraded(cmd, args, "none");
        }
        default:
            return make_degraded(cmd, args, "none");
    }
}

bool SandboxAdapter::is_enabled() {
#if defined(__APPLE__) || defined(__linux__)
    return SandboxDetector::instance().is_available();
#else
    return false;
#endif
}

WrappedCommand SandboxAdapter::wrap_with_seatbelt(const std::string& sandbox_exec_path,
                                                  const std::string& cmd,
                                                  const std::vector<std::string>& args,
                                                  const SandboxConfig& config) {
#if defined(__APPLE__)
    WrappedCommand result;
    result.cmd = sandbox_exec_path;
    result.was_wrapped = true;
    result.degraded = false;
    result.backend_name = "seatbelt";

    // sandbox-exec -p '<profile>' -- <cmd> <args...>
    result.args.push_back("-p");
    result.args.push_back(generate_seatbelt_profile(config));
    result.args.push_back("--");
    result.args.push_back(cmd);
    for (const auto& a : args) {
        result.args.push_back(a);
    }

    return result;
#else
    (void)sandbox_exec_path;
    (void)config;
    return make_degraded(cmd, args, "none");
#endif
}

WrappedCommand SandboxAdapter::wrap_with_bubblewrap(const std::string& bwrap_path,
                                                    const std::string& cmd,
                                                    const std::vector<std::string>& args,
                                                    const SandboxConfig& config) {
#if defined(__linux__)
    WrappedCommand result;
    result.cmd = bwrap_path;
    result.was_wrapped = true;
    result.degraded = false;
    result.backend_name = "bubblewrap";

    // bwrap [options] -- <cmd> <args...>
    auto bwrap_args = generate_bwrap_args(config);
    result.args = std::move(bwrap_args);
    result.args.push_back("--");
    result.args.push_back(cmd);
    for (const auto& a : args) {
        result.args.push_back(a);
    }

    return result;
#else
    (void)bwrap_path;
    (void)config;
    return make_degraded(cmd, args, "none");
#endif
}

WrappedCommand SandboxAdapter::make_degraded(const std::string& cmd,
                                             const std::vector<std::string>& args,
                                             const std::string& backend_name) {
    WrappedCommand result;
    result.cmd = cmd;
    result.args = args;
    result.was_wrapped = false;
    result.degraded = true;
    result.backend_name = backend_name;
    return result;
}

WrappedCommand SandboxAdapter::make_passthrough(const std::string& cmd,
                                                const std::vector<std::string>& args) {
    WrappedCommand result;
    result.cmd = cmd;
    result.args = args;
    result.was_wrapped = false;
    result.degraded = false;
    result.backend_name = "none";
    return result;
}

}  // namespace agent::process::sandbox
