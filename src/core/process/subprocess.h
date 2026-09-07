/**
 * @file subprocess.h
 * @brief subprocess::exec() — 跨平台子进程执行封装
 * @details 在 C++ 程序中启动外部命令（如 rg / git / bash），捕获 stdout/stderr，
 *          支持超时与取消。C++ 标准库无此能力，需封装操作系统进程 API：
 *          - Windows: CreateProcessW + CreatePipe + ReadFile
 *          - POSIX:   fork + execvp + pipe + poll
 *
 *          设计为函数而非类：Workx 初期只需同步执行，不需要 background/streaming，
 *          per-call 的 exec() 函数足够（对齐 Claude Code CLI 的 ripgrep 调用模式）。
 *
 *          不做"进程管理器"：进程每次新建不复用，取消用回调而非 AbortSignal 链，
 *          孤儿进程防护由内部保证（超时/取消时一定 kill）。
 *          详见 src/agent/tool/README.md "外部工具集成"章节。
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/process/exec_output.h"
#include "core/utils/result_v2.h"

namespace agent::process {

/// @brief exec() 调用选项
struct ExecOptions {
    std::string cwd;                                    ///< 工作目录（空表示继承父进程）
    std::vector<std::string> args;                      ///< 命令行参数（不含命令本身）
    std::optional<std::chrono::milliseconds> timeout;   ///< 超时（不设则无超时）
    std::function<bool()> is_cancelled;                 ///< 取消检查回调（返回 true 表示已取消）

    /// @brief stdout 缓冲区上限（字节），超限截断并置 truncated 标志
    /// @details 防止恶意/失控子进程写爆内存。默认 20MB（对齐 CC ripgrep.ts MAX_BUFFER_SIZE）
    size_t max_output_bytes = 20 * 1024 * 1024;
};

/// @brief 同步执行外部命令，捕获输出
/// @param cmd  可执行文件路径（绝对路径或 PATH 中可找到的命令名）
/// @param opts 选项（cwd / args / timeout / is_cancelled）
/// @return ResultV2<ExecOutput>：
///         - ok: 子进程已执行（无论退出码如何），含 stdout/stderr/exit_code
///         - err: 启动失败（命令不存在、管道创建失败等），不含子进程输出
///
/// @par 超时/取消行为
/// - POSIX: kill(SIGTERM) → 5s 后 kill(SIGKILL)（防 rg 卡在不可中断 I/O）
/// - Windows: TerminateProcess（无 SIGTERM 概念，直接终止）
/// - 对齐 Claude Code CLI utils/ripgrep.ts L174-182 的升级逻辑
///
/// @par 线程安全
/// 函数本身无状态，可被多个线程并行调用。每次调用启动独立子进程。
///
/// @par 编码
/// - POSIX: stdout/stderr 原样透传（通常 UTF-8）
/// - Windows: 子进程输出按 CP_UTF8 解码，非 UTF-8 回退按当前 ACP 转换
ResultV2<ExecOutput> exec(const std::string& cmd, const ExecOptions& opts);

/// @brief 交互式子进程执行结果
struct InteractiveExecResult {
    int exit_code = 0;  ///< 退出码（启动失败时无意义）
};

/// @brief 同步执行交互式命令（继承终端 stdio，不捕获输出）
/// @details 供内嵌外部交互程序使用（如 /edit 拉起 nvim）：
///          - Windows: CreateProcessW 不设置 STARTF_USESTDHANDLES，子进程继承父进程控制台
///          - POSIX:   fork + execvp 不重定向 stdio，子进程留在前台进程组（可接收终端信号）
/// @param cmd  可执行文件路径（绝对路径或 PATH 中可找到的命令名）
/// @param args 命令行参数（不含命令本身）
/// @param cwd  工作目录（空表示继承父进程）
/// @return ok: 子进程已执行并退出（含退出码）；err: 启动失败
ResultV2<InteractiveExecResult> exec_interactive(const std::string& cmd,
                                                 const std::vector<std::string>& args,
                                                 const std::string& cwd = {});

} // namespace agent::process
