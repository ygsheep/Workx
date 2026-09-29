/**
 * @file exec_output.h
 * @brief ExecOutput — 外部命令执行结果
 * @details subprocess::exec() 的返回值，封装 stdout/stderr/exit_code。
 *          设计为 POD 风格结构体，无行为逻辑，便于工具层转换为 ToolResult。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace agent::process {

/// @brief 外部命令执行结果
/// @details 封装子进程的 stdout/stderr 文本输出与退出码。
///          - 成功执行：exit_code 为子进程退出码（0 通常表示成功）
///          - 超时：timed_out = true，exit_code 可能为 -1（被强制终止）
///          - 取消：cancelled = true，exit_code 可能为 -1（被强制终止）
///          - 输出截断：stdout_truncated/stderr_truncated = true，输出超过 max_output_bytes
struct ExecOutput {
    int exit_code = -1;       ///< 子进程退出码（-1 表示未正常退出，如被 kill）
    std::string stdout_text;  ///< stdout 内容（UTF-8）
    std::string stderr_text;  ///< stderr 内容（UTF-8）
    bool timed_out = false;   ///< 是否因超时被终止
    bool cancelled = false;   ///< 是否因取消被终止
    bool stdout_truncated = false;  ///< stdout 是否因超过 max_output_bytes 被截断
    bool stderr_truncated = false;  ///< stderr 是否因超过 max_output_bytes 被截断

    /// @brief 是否成功执行（exit_code == 0 且未超时/取消）
    [[nodiscard]] bool is_success() const noexcept {
        return exit_code == 0 && !timed_out && !cancelled;
    }
};

}  // namespace agent::process
