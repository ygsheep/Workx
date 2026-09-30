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

#include <optional>
#include <string>
#include <vector>

#include "core/process/process_isolation.h"
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

    /// @brief 进程级隔离规格（#84 方案 B）
    /// @details 与文件系统/网络策略**正交**：非空时表示"即使沙箱后端不可用，
    ///          也应在 Windows 上用 Job Object 约束进程树与资源"。
    ///          因此 Windows 上可以是 `degraded == true` 且本字段非空 —— 语义是
    ///          "没有 FS/网络隔离，但有进程级约束"，调用方应据此如实上报，
    ///          而不是笼统地说"完全没有隔离"。
    std::optional<ProcessIsolationSpec> isolation;
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

    /// @brief 从沙盒配置派生进程级隔离规格（#84 方案 B）
    /// @param config 沙盒规则配置
    /// @return 宽松配置返回 nullopt；严格配置返回进程树与资源约束
    /// @details 与文件系统/网络策略**正交**：本函数只产出"进程树 + 资源"层面的约束。
    ///          上限取值刻意宽松 —— 目的是"防失控"而非"精确配额"：过紧会让正常的
    ///          多进程构建（MSBuild -j / npm install）直接失败，那比不做更糟。
    ///          POSIX 侧该结果会被 process::exec() 忽略（进程组机制已覆盖进程树终止）。
    static std::optional<ProcessIsolationSpec> derive_isolation(const SandboxConfig& config);

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
    /// @param isolation 可选的进程级隔离规格：Windows 上即使沙盒后端缺失，
    ///                  Job Object 仍能提供进程树与资源约束（#84 方案 B）
    static WrappedCommand make_degraded(
        const std::string& cmd, const std::vector<std::string>& args,
        const std::string& backend_name,
        std::optional<ProcessIsolationSpec> isolation = std::nullopt);

    /// 生成宽松结果（返回原命令，不标记降级）
    static WrappedCommand make_passthrough(const std::string& cmd,
                                           const std::vector<std::string>& args);
};

}  // namespace agent::process::sandbox
