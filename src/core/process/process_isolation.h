/**
 * @file process_isolation.h
 * @brief ProcessIsolationSpec — 进程级隔离规格（#84 方案 B）
 * @details 描述"执行子进程时需要的进程树与资源约束"。它与 SandboxConfig 的
 *          文件系统/网络规则**正交**，二者不是替代关系：
 *          - 本结构只承载进程级能力（Windows 上用 Job Object 实现）；
 *          - 按路径拦 FS、按域名断网需要 AppContainer 之类的机制，不在本结构范围内。
 *
 * @par 为什么需要单独的结构
 * SandboxAdapter::wrap_command() 是"纯字符串重写"接口，而 Job Object 无法用改写
 * 命令行表达 —— 它必须在建进程时参与（CREATE_SUSPENDED → AssignProcessToJobObject
 * → ResumeThread）。因此把"策略来源"留在 SandboxAdapter，"执行机制"下沉到 process 层，
 * 本结构即两者之间的载体：
 * ```cpp
 * auto wrapped = SandboxAdapter::wrap_command(shell, args, config);
 * opts.isolation = wrapped.isolation;   // 策略侧派生
 * auto result = process::exec(wrapped.cmd, opts);  // 机制侧消费
 * ```
 *
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <cstdint>

namespace agent::process {

/// @brief 进程级隔离规格（平台中立）
/// @details 每个字段的 0 值表示"该项不限制"，便于调用方只开启自己关心的约束。
struct ProcessIsolationSpec {
    /// @brief 进程树内最大活跃进程数（0 = 不限制）
    /// @note Windows 映射为 JOB_OBJECT_LIMIT_ACTIVE_PROCESS；POSIX 无对应机制
    uint32_t max_processes = 0;

    /// @brief 单个进程的内存上限（字节，0 = 不限制）
    /// @note Windows 映射为 JOB_OBJECT_LIMIT_PROCESS_MEMORY；POSIX 无对应机制
    uint64_t max_process_memory_bytes = 0;

    /// @brief 命令结束时是否连带终止整棵进程树
    /// @details Windows 映射为 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE（正常结束、超时、
    ///          取消三种收尾路径都覆盖）。这是本结构的核心收益：TerminateProcess
    ///          只终止直接子进程，`cmd.exe /c <命令>` 派生的孙进程会残留成孤儿。
    ///          POSIX 侧本项已由 exec_posix 的进程组机制（setpgid + kill(-pid)）覆盖。
    bool kill_tree_on_exit = true;

    /// @brief 是否真的约束了什么（三项均为默认值时返回 false）
    [[nodiscard]] bool is_meaningful() const noexcept {
        return max_processes > 0 || max_process_memory_bytes > 0 || kill_tree_on_exit;
    }
};

/// @brief 进程级隔离的实际应用结果
/// @details 由 subprocess::exec() 填入 ExecOutput，供上层决定是否告警与留痕。
enum class IsolationOutcome {
    NotApplicable,  ///< 本次未走 Job Object 路径（未请求，或平台由进程组机制覆盖）
    Applied,        ///< 请求了且已生效
    Failed,  ///< 请求了但未能生效（如父进程已在 Job 内且系统不支持嵌套）
};

}  // namespace agent::process
