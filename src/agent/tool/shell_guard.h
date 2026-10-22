/**
 * @file shell_guard.h
 * @brief Shell 命令安全守卫（#35：危险命令 / SSRF / env 泄露 / cwd 限制）
 * @details 纯函数无状态，BashTool / PowerShellTool 共用的命令级安全过滤。
 *          命中任一风险模式返回对应布尔，调用方决定拦截或 AskUser 确认。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace agent::tool {

/// @brief 命令风险类别（可叠加）
enum class ShellRisk : uint32_t {
    None = 0,
    Destructive = 1u << 0,  ///< 破坏性命令（rm -rf /、mkfs、dd、format、shutdown 等）
    SSRF = 1u << 1,  ///< 请求内网/云元数据地址（curl/wget/iwr → 169.254.x 等）
    EnvLeak =
        1u << 2,  ///< 输出环境变量（env / printenv / /proc/<pid>/environ / Get-ChildItem env:）
};

/// @brief ShellRisk 位运算符（组合/检测风险类别）
inline constexpr ShellRisk operator|(ShellRisk a, ShellRisk b) noexcept {
    return static_cast<ShellRisk>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr ShellRisk operator&(ShellRisk a, ShellRisk b) noexcept {
    return static_cast<ShellRisk>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline constexpr ShellRisk& operator|=(ShellRisk& a, ShellRisk b) noexcept {
    a = a | b;
    return a;
}
inline constexpr bool operator!(ShellRisk a) noexcept { return a == ShellRisk::None; }

/// @brief 检测命令的风险级别（多个命中按位或）
ShellRisk detect_shell_risk(std::string_view command);

/// @brief 检测命令是否包含破坏性操作
bool contains_destructive_command(std::string_view command);

/// @brief 检测命令是否请求内网/元数据地址（SSRF）
bool is_ssrf_target(std::string_view command);

/// @brief 检测命令是否泄露环境变量
bool leaks_env_vars(std::string_view command);

/// @brief 沙盒目标级风险类别（可叠加，#84 方案 A）
enum class SandboxTargetRisk : uint32_t {
    None = 0,
    SystemWrite = 1u << 0,    ///< 敏感系统路径写意图（C:\Windows、Program Files、/etc 等）
    RegistryWrite = 1u << 1,  ///< HKLM 注册表写意图
    NetworkUse = 1u << 2,     ///< 网络访问意图（network_isolated 全禁 / 白名单外域名）
};

/// @brief SandboxTargetRisk 位运算符（组合/检测风险类别）
inline constexpr SandboxTargetRisk operator|(SandboxTargetRisk a, SandboxTargetRisk b) noexcept {
    return static_cast<SandboxTargetRisk>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
inline constexpr SandboxTargetRisk operator&(SandboxTargetRisk a, SandboxTargetRisk b) noexcept {
    return static_cast<SandboxTargetRisk>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}
inline constexpr bool operator!(SandboxTargetRisk a) noexcept { return a == SandboxTargetRisk::None; }

/// @brief 沙盒目标级检测的策略输入（由调用方从 SandboxConfig 派生）
struct SandboxTargetPolicy {
    /// 完全隔离网络（对应 SandboxConfig::network_isolated）
    bool network_isolated = true;
    /// 域名白名单（对应 SandboxConfig::allow_domains）：非空时仅拦截白名单外 URL，
    /// 纯工具名启发式（curl 等）不再触发——无法从命令判断目标域时从严会误伤白名单语义
    std::vector<std::string> allow_domains;
    /// 额外敏感写路径前缀（对应 SandboxConfig::deny_write，与内置敏感路径同权）
    std::vector<std::string> deny_write;
};

/// @brief 检测命令的沙盒目标级风险（#84 方案 A：命令级前置拦截）
/// @details Windows 无 FS/网络沙盒后端，本函数是 restrictive 档的执行前兜底；
///          macOS/Linux 上与 seatbelt/bwrap 构成纵深（运行期沙盒仍做真实拦截）。
///          词法启发式，无 AST：敏感路径写意图要求「写动作 token 与敏感路径
///          近邻共现」（窗口 64 字符），纯读（type/cat/dir 敏感路径）不拦截。
/// @param command 待检命令
/// @param policy 策略输入（network_isolated / allow_domains / deny_write）
SandboxTargetRisk detect_sandbox_target_risk(std::string_view command,
                                             const SandboxTargetPolicy& policy);

/// @brief 沙盒目标级风险的人类可读描述（供确认弹窗/审计展示）
/// @return 空字符串表示无风险；否则为逗号分隔的风险说明
std::string sandbox_target_risk_description(SandboxTargetRisk risk);

/// @brief 校验 BASH/PowerShell 的 cwd 参数是否合法
/// @param cwd 请求的工作目录（绝对路径）
/// @param base 项目根目录（ctx.cwd）
/// @param allowlist 额外允许的范围（可选）
/// @return 合法返回 true；非法（空、非绝对、越界）返回 false
bool is_command_cwd_allowed(std::string_view cwd, std::string_view base,
                            const std::vector<std::string>& allowlist = {});

/// @brief 命令风险的人类可读描述（供 AskUser 确认时展示）
/// @return 空字符串表示无风险；否则为逗号分隔的风险说明
std::string shell_risk_description(ShellRisk risk);

}  // namespace agent::tool