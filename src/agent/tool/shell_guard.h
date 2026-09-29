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