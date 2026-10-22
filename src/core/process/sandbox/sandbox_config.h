/**
 * @file sandbox_config.h
 * @brief SandboxConfig — 沙盒规则配置数据结构
 * @details 描述子进程在沙盒内的文件系统与网络访问规则。
 *          由 SandboxAdapter 根据当前平台（macOS Seatbelt / Linux bwrap）
 *          翻译为对应的 profile 字符串或命令行参数。
 *
 *          设计为纯数据结构（无虚函数、无业务逻辑），便于序列化与热更新。
 *          对齐 Claude Code CLI 的 SandboxRuntimeConfig（utils/sandbox/sandbox-adapter.ts）。
 *
 * @par 路径匹配语义
 * 规则中的路径按"前缀匹配"判定：若路径以规则字符串开头则命中。
 * 例如 allow_read=["/usr"] 匹配 /usr/bin/rg、/usr/lib/libc.so 等。
 * deny 优先级高于 allow：若同时命中，deny 生效。
 *
 * @par 域名匹配语义
 * 支持 `*` 通配符：`*.example.com` 匹配 `api.example.com`、`www.example.com`。
 * 精确域名（无 `*`）仅匹配该域名本身。端口默认匹配 443（HTTPS）。
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

namespace agent::process::sandbox {

/// @brief 沙盒规则配置
/// @details 纯数据结构，描述文件系统与网络访问规则。由调用方填充后传给
///          SandboxAdapter::wrap_command() 生成平台 profile。
struct SandboxConfig {
    // === 文件系统规则 ===
    std::vector<std::string> allow_write;  ///< 允许写入的路径前缀（deny 优先）
    std::vector<std::string> deny_write;   ///< 拒绝写入的路径前缀
    std::vector<std::string> allow_read;   ///< 允许读取的路径前缀
    std::vector<std::string> deny_read;    ///< 拒绝读取的路径前缀

    // === 网络规则 ===
    std::vector<std::string> allow_domains;  ///< 允许的网络域名（支持 `*` 通配符）
    std::vector<std::string> deny_domains;   ///< 拒绝的网络域名
    bool network_isolated = true;  ///< 是否完全隔离网络（true 时忽略 allow_domains）

    // === 系统目录默认策略 ===
    /// 是否允许读取系统目录（/usr /lib /lib64 /etc /bin /sbin）
    /// @details 默认 true：子进程需要读取共享库和系统命令才能运行
    bool allow_system_read = true;

    /// 是否允许写入临时目录（/tmp 及 TMPDIR）
    /// @details 默认 true：许多工具（如 rg 缓存）需要写临时文件
    bool allow_temp_write = true;

    /// @brief 构建严格模式配置：仅允许 cwd 读写，隔离网络
    /// @param cwd 工作目录（沙盒内允许读写的根）
    /// @return SandboxConfig 预填充系统目录读权限 + cwd 读写权限
    /// @note 调用方可在返回值上追加 allow_domains 以放开特定网络访问
    static SandboxConfig restrictive(const std::string& cwd);

    /// @brief 构建宽松配置：允许全盘读写、不隔离网络
    /// @return SandboxConfig 所有规则为空、network_isolated=false
    /// @note 用于 dangerously_disable_sandbox 场景或可信命令
    static SandboxConfig permissive();

    /// @brief 判断是否为宽松配置（无任何限制）
    [[nodiscard]] bool is_permissive() const noexcept;
};

}  // namespace agent::process::sandbox
