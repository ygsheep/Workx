/**
 * @file sandbox_adapter.h
 * @brief SandboxAdapter — 命令包装适配器
 * @details 在 subprocess::exec() 之前将原命令包装为带沙盒前缀的命令：
 *          - macOS: `sandbox-exec -p '<profile>' -- <cmd> <args...>`
 *          - Linux: `bwrap [options] -- <cmd> <args...>`
 *          - Windows: 无包装（降级模式，返回原命令）
 *
 *          设计为静态工具类（无状态），所有方法均为 static。
 *          平台 profile 生成逻辑封装在 .cpp 内部的平台条件编译块中。
 *
 * @par 与 subprocess 的配合
 * ```cpp
 * SandboxConfig config = SandboxConfig::restrictive(ctx.cwd);
 * auto wrapped = SandboxAdapter::wrap_command("rg", {"pattern", "."}, config);
 * ExecOptions opts;
 * opts.args = wrapped.args;
 * // opts.cwd / timeout / ... 由调用方填充
 * auto result = subprocess::exec(wrapped.cmd, opts);
 * ```
 *
 * @par 降级策略
 * 当沙盒后端不可用（Windows 或工具未安装）时，wrap_command() 返回原命令并标记
 * `degraded=true`。调用方应检查此标志并决定是否：
 *   1. 继续执行（信任命令）；
 *   2. 拒绝执行（安全敏感场景）；
 *   3. 提示用户安装沙盒工具。
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

#include "core/process/sandbox/sandbox_config.h"

namespace agent::process::sandbox {

/// @brief 包装后的命令
/// @details wrap_command() 的返回值，包含包装后的命令和元信息
struct WrappedCommand {
    std::string cmd;  ///< 包装后的命令（可能是 bwrap/sandbox-exec 路径，或原命令）
    std::vector<std::string> args;  ///< 包装后的参数列表
    bool was_wrapped = false;       ///< 是否实际包装（false 表示降级或宽松配置）
    bool degraded = false;          ///< 是否降级（沙盒工具缺失，返回原命令）
    std::string backend_name;  ///< 使用的后端名称（"seatbelt" / "bubblewrap" / "none"）
};

/// @brief 沙盒命令包装适配器
/// @details 静态工具类，根据平台和配置将原命令包装为沙盒命令
class SandboxAdapter {
   public:
    /// @brief 包装命令
    /// @param cmd 原始命令（如 "rg"、"git"）
    /// @param args 原始参数列表
    /// @param config 沙盒规则配置
    /// @return WrappedCommand 包装后的命令
    ///
    /// @par 行为
    /// - 若 config.is_permissive()，直接返回原命令（was_wrapped=false, degraded=false）
    /// - 若沙盒后端可用，生成 profile 并返回包装命令（was_wrapped=true）
    /// - 若沙盒后端不可用，返回原命令并标记 degraded=true
    static WrappedCommand wrap_command(const std::string& cmd, const std::vector<std::string>& args,
                                       const SandboxConfig& config);

    /// @brief 沙盒是否启用（编译期 + 运行期双重判定）
    /// @details 编译期：Windows 直接返回 false
    ///          运行期：macOS/Linux 检查沙盒工具是否可用
    static bool is_enabled();

   private:
    // 平台特定的包装实现（在 .cpp 中条件编译）
    static WrappedCommand wrap_with_seatbelt(const std::string& sandbox_exec_path,
                                             const std::string& cmd,
                                             const std::vector<std::string>& args,
                                             const SandboxConfig& config);

    static WrappedCommand wrap_with_bubblewrap(const std::string& bwrap_path,
                                               const std::string& cmd,
                                               const std::vector<std::string>& args,
                                               const SandboxConfig& config);

    /// 生成降级结果（返回原命令）
    static WrappedCommand make_degraded(const std::string& cmd,
                                        const std::vector<std::string>& args,
                                        const std::string& backend_name);

    /// 生成宽松结果（返回原命令，不标记降级）
    static WrappedCommand make_passthrough(const std::string& cmd,
                                           const std::vector<std::string>& args);
};

}  // namespace agent::process::sandbox
