/**
 * @file subprocess.cpp
 * @brief subprocess::exec() 跨平台实现
 * @details
 * Windows 实现：
 *   - CreatePipe 创建 stdout/stderr 管道（继承到子进程，父进程读端）
 *   - CreateProcessW 启动子进程
 *   - ReadFile 循环读取管道 + 检查超时/取消
 *   - 终止：默认 TerminateProcess（只杀直接子进程）；调用方要求进程级隔离
 *     （ExecOptions::isolation，#84 方案 B）时改走 TerminateJobObject 连带杀全树
 *   - 编码：子进程输出按 UTF-8 解码，回退 MultiByteToWideChar(CP_ACP)
 *
 * POSIX 实现：
 *   - pipe() 创建 stdout/stderr 管道
 *   - fork() + execvp() 启动子进程
 *   - poll() 等待管道可读 + 检查超时/取消
 *   - kill(SIGTERM) → 5s 后 kill(SIGKILL) 升级（防卡在不可中断 I/O）
 *   - 编码：透传（通常 UTF-8）
 *
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/process/subprocess.h"

#include <algorithm>
#include <cstring>
#include <thread>

#ifdef _WIN32
// Job Object API（jobapi2.h）随 windows.h 一并引入；WIN32_LEAN_AND_MEAN 只排除
// winsock / ole / cryptography 等无关子系统，可显著缩短编译时间。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent::process {
namespace {

/// @brief 验证字节序列是否为合法 UTF-8
/// @return true=合法 UTF-8（含纯 ASCII）；false=含非法字节
/// @details 与 encoding.cpp 的 validate_utf8 同源逻辑，core 层独立实现以避免分层倒置
bool is_valid_utf8(const std::string& s) noexcept {
    const size_t n = s.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            ++i;
        } else if (c < 0xC2) {
            return false;  // 非法首字节
        } else if (c < 0xE0) {
            if (i + 1 >= n || (static_cast<unsigned char>(s[i + 1]) & 0xC0) != 0x80) return false;
            i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= n || (static_cast<unsigned char>(s[i + 1]) & 0xC0) != 0x80 ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0) != 0x80)
                return false;
            i += 3;
        } else if (c < 0xF5) {
            if (i + 3 >= n || (static_cast<unsigned char>(s[i + 1]) & 0xC0) != 0x80 ||
                (static_cast<unsigned char>(s[i + 2]) & 0xC0) != 0x80 ||
                (static_cast<unsigned char>(s[i + 3]) & 0xC0) != 0x80)
                return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

/// @brief 把非法 UTF-8 字节序列清洗为合法 UTF-8（替换非法字节为 '?'）
/// @details 仅在 is_valid_utf8 返回 false 时调用；
///          逐字节扫描，遇到非法首字节或截断的多字节序列时替换为 '?'。
///          策略保守：宁可替换也不引入错误解码，确保后续 JSON 序列化不再抛 type_error.316。
std::string sanitize_invalid_utf8(std::string s) {
    const size_t n = s.size();
    size_t i = 0;
    std::string out;
    out.reserve(n);
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += static_cast<char>(c);
            ++i;
        } else if (c < 0xC2) {
            out += '?';
            ++i;
        } else if (c < 0xE0) {
            if (i + 1 < n && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
                out += s[i];
                out += s[i + 1];
                i += 2;
            } else {
                out += '?';
                ++i;
            }
        } else if (c < 0xF0) {
            if (i + 2 < n && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80) {
                out += s[i];
                out += s[i + 1];
                out += s[i + 2];
                i += 3;
            } else {
                out += '?';
                ++i;
            }
        } else if (c < 0xF5) {
            if (i + 3 < n && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(s[i + 2]) & 0xC0) == 0x80 &&
                (static_cast<unsigned char>(s[i + 3]) & 0xC0) == 0x80) {
                out += s[i];
                out += s[i + 1];
                out += s[i + 2];
                out += s[i + 3];
                i += 4;
            } else {
                out += '?';
                ++i;
            }
        } else {
            out += '?';
            ++i;
        }
    }
    return out;
}

/// @brief 清洗子进程输出为合法 UTF-8
/// @details 策略（对齐文件头注释承诺的"按 UTF-8 解码，回退 CP_ACP"）：
///          1. 合法 UTF-8：原样返回（最常见路径，零开销）
///          2. 非法 UTF-8：
///             - Windows：先尝试 CP_ACP（系统 ANSI 代码页，中文系统为 CP_936/GBK）
///                       → UTF-16 → UTF-8 的转换；失败则回退到字节级 sanitize（替换非法字节为 '?'）
///             - POSIX：直接字节级 sanitize（POSIX 系统通常已 UTF-8，走到这里属罕见情况）
/// @note 必须确保返回值能被 nlohmann::json 序列化而不抛 type_error.316
std::string sanitize_output_to_utf8(std::string s) {
    if (s.empty() || is_valid_utf8(s)) return s;

#ifdef _WIN32
    // 尝试 CP_ACP（系统 ANSI 代码页）→ UTF-16 → UTF-8
    const int wlen =
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (wlen > 0) {
        std::wstring wstr(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), wstr.data(), wlen);
        const int ulen =
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, nullptr, 0, nullptr, nullptr);
        if (ulen > 0) {
            std::string result(static_cast<size_t>(ulen), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, result.data(), ulen, nullptr,
                                nullptr);
            // 转换成功则使用解码结果；转换结果仍含非法字节则再清洗一次（保险）
            return is_valid_utf8(result) ? result : sanitize_invalid_utf8(std::move(result));
        }
    }
#endif
    // 最后的兜底：字节级清洗
    return sanitize_invalid_utf8(std::move(s));
}

}  // anonymous namespace
}  // namespace agent::process

namespace agent::process {

namespace {

#ifdef _WIN32
// ============================================================
// Windows 实现
// ============================================================

/// Auto-handle：RAII 包装 Windows HANDLE，析构时 CloseHandle
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : h(handle) {}
    ~HandleGuard() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    operator HANDLE() const { return h; }
    HANDLE* operator&() { return &h; }
    /// 释放所有权（返回 handle，不再负责关闭）
    HANDLE release() {
        HANDLE tmp = h;
        h = INVALID_HANDLE_VALUE;
        return tmp;
    }
};

/// Job Object 句柄的 RAII 包装（无效值为 nullptr，与 HandleGuard 不同）
/// @details 析构即 CloseHandle(job)。若 job 带 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE，
///          关闭最后一个句柄会连带终止 job 内残留进程 —— 这正是"命令结束后不留
///          孤儿孙进程"的实现方式，故析构顺序（先 job 后 process）不可调换。
struct JobGuard {
    HANDLE h = nullptr;
    explicit JobGuard(HANDLE handle = nullptr) : h(handle) {}
    ~JobGuard() {
        if (h != nullptr) CloseHandle(h);
    }
    JobGuard(const JobGuard&) = delete;
    JobGuard& operator=(const JobGuard&) = delete;
    operator HANDLE() const { return h; }
};

/// 创建匿名管道，读端不继承（父进程持有），写端可继承（子进程持有）
bool create_inheritable_pipe(HANDLE* read_handle, HANDLE* write_handle) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(read_handle, write_handle, &sa, 0)) {
        return false;
    }
    // 读端设为不可继承，避免子进程持有导致管道无法关闭
    if (!SetHandleInformation(*read_handle, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(*read_handle);
        CloseHandle(*write_handle);
        return false;
    }
    return true;
}

/// 将 UTF-8 字符串转换为 UTF-16 宽字符串
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

/// 将宽字符串按 UTF-8 编码转回窄字符串
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

/// 从管道读取数据（非阻塞模式）
/// 返回值：true 表示读到了数据，false 表示暂时无数据或 EOF（调用方需另行检测 EOF）
/// @details 先用 PeekNamedPipe 探测数据量；若 Peek 失败（管道写端已关闭 = broken pipe），
///          仍尝试 ReadFile，因为管道缓冲区可能还有未读数据（ReadFile 在 broken pipe
///          上会立即返回剩余数据或失败，不会阻塞）。
bool read_pipe(HANDLE pipe, std::string& buf, size_t max_bytes) {
    if (buf.size() >= max_bytes) return false;

    DWORD available = 0;
    bool peek_ok = PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr);

    // Peek 成功但无数据：管道仍开着，只是暂时没数据
    if (peek_ok && available == 0) return false;

    // Peek 成功且有数据：按 available 量读
    // Peek 失败（broken pipe）：试读 8192 字节，可能拿到缓冲区剩余数据
    DWORD to_read = static_cast<DWORD>(
        std::min<size_t>((peek_ok && available > 0) ? available : 8192, max_bytes - buf.size()));
    if (to_read == 0) return false;

    std::string chunk(to_read, '\0');
    DWORD read_bytes = 0;
    if (!ReadFile(pipe, chunk.data(), to_read, &read_bytes, nullptr) || read_bytes == 0) {
        return false;  // 真正 EOF（无更多数据）
    }
    chunk.resize(read_bytes);
    buf += chunk;
    return true;
}

/// 判断参数是否需要加引号（含空格/制表符/引号时需要）
bool needs_quotes(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '"') return true;
    }
    return false;
}

/// 转义单个参数：需要时加引号并转义内部引号
/// @details 遵循 CommandLineToArgvW 规则：每个 " 变成 \"，整体用 "..." 包裹
std::string escape_arg(const std::string& arg) {
    if (!needs_quotes(arg)) return arg;
    std::string result;
    result.reserve(arg.size() + 4);
    result += '"';
    for (char c : arg) {
        if (c == '"')
            result += "\\\"";
        else
            result += c;
    }
    result += '"';
    return result;
}

/// 创建并配置进程级隔离用的 Job Object（#84 方案 B）
/// @param spec 隔离规格
/// @return 配置成功的 job 句柄（调用方负责关闭）；无任何约束可施加或配置失败时返回 nullptr
/// @details 三项约束分别映射到 JOB_OBJECT_LIMIT_ACTIVE_PROCESS（进程数）、
///          JOB_OBJECT_LIMIT_PROCESS_MEMORY（单进程内存）、
///          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE（关闭句柄即连带终止，覆盖正常结束/超时/取消）。
HANDLE create_isolation_job(const ProcessIsolationSpec& spec) {
    HandleGuard job(CreateJobObjectW(nullptr, nullptr));
    if (job.h == nullptr) {
        return nullptr;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (spec.max_processes > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = spec.max_processes;
    }
    if (spec.max_process_memory_bytes > 0) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        limits.ProcessMemoryLimit = static_cast<SIZE_T>(spec.max_process_memory_bytes);
    }
    if (spec.kill_tree_on_exit) {
        limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    }
    if (limits.BasicLimitInformation.LimitFlags == 0) {
        return nullptr;  // 规格为空，无可施加的约束
    }
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return nullptr;
    }
    return job.release();
}

/// 把挂起态的子进程指派到新建的 Job Object 并恢复运行
/// @param spec 隔离规格
/// @param process 子进程句柄（由 CreateProcessW 以 CREATE_SUSPENDED 创建）
/// @param thread 该进程主线程句柄
/// @param[out] job 生效时写入 job 句柄（调用方负责关闭），未生效时保持 nullptr
/// @return Applied / Failed
/// @note **降级不是致命错误**：命令照常执行，只是失去进程树与资源约束，
///       由上层（BashTool 等）决定是否告警。常见触发场景是父进程本身已在某个
///       Job 内且系统不支持嵌套 Job（Windows 7）。
IsolationOutcome apply_process_isolation(const ProcessIsolationSpec& spec, HANDLE process,
                                         HANDLE thread, HANDLE* job) {
    *job = nullptr;
    HandleGuard new_job(create_isolation_job(spec));
    IsolationOutcome outcome = IsolationOutcome::Failed;
    if (new_job.h != nullptr && AssignProcessToJobObject(new_job.h, process)) {
        *job = new_job.release();
        outcome = IsolationOutcome::Applied;
    }

    if (ResumeThread(thread) == static_cast<DWORD>(-1)) {
        // 理论上不可达（thread 句柄刚由 CreateProcessW 返回）。万一发生，子进程会
        // 永久挂起并把读取循环拖死，故宁可终止也不要放任。
        TerminateProcess(process, 1);
        if (*job != nullptr) {
            CloseHandle(*job);
            *job = nullptr;
        }
        return IsolationOutcome::Failed;
    }
    return outcome;
}

/// 终止子进程
/// @param process 子进程句柄
/// @param job Job Object 句柄（nullptr 表示本次没有进程级隔离）
/// @details 有 job 时走 TerminateJobObject 连带终止整棵树 —— TerminateProcess 只杀
///          直接子进程，`cmd.exe /c <命令>` 派生的孙进程会残留成孤儿（#84）。
void terminate_child(HANDLE process, HANDLE job) {
    if (job != nullptr) {
        TerminateJobObject(job, 1);
    } else {
        TerminateProcess(process, 1);
    }
}

ResultV2<ExecOutput> exec_windows(const std::string& cmd, const ExecOptions& opts) {
    // 1. 构建命令行：cmd + arg1 + arg2 + ...
    //    只对含空格/引号的参数加引号，避免 cmd.exe "/c" 被引号包裹后无法识别
    //    （cmd.exe 不识别 "/c" 为 /c 开关，会导致命令解析失败）
    std::string cmdline = escape_arg(cmd);
    for (const auto& arg : opts.args) {
        cmdline += ' ';
        cmdline += escape_arg(arg);
    }
    std::wstring wcmdline = utf8_to_wide(cmdline);
    std::wstring wcwd = utf8_to_wide(opts.cwd);

    // 2. 创建 stdout/stderr 管道
    HANDLE stdout_read = INVALID_HANDLE_VALUE, stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_read = INVALID_HANDLE_VALUE, stderr_write = INVALID_HANDLE_VALUE;
    if (!create_inheritable_pipe(&stdout_read, &stdout_write)) {
        return ResultV2<ExecOutput>::err(Error::Code::InternalError, "CreatePipe(stdout) failed",
                                         "subprocess::exec");
    }
    HandleGuard g_stdout_read(stdout_read), g_stdout_write(stdout_write);
    if (!create_inheritable_pipe(&stderr_read, &stderr_write)) {
        return ResultV2<ExecOutput>::err(Error::Code::InternalError, "CreatePipe(stderr) failed",
                                         "subprocess::exec");
    }
    HandleGuard g_stderr_read(stderr_read), g_stderr_write(stderr_write);

    // 3. 启动子进程
    //    #84 方案 B：要求进程级隔离时以挂起态创建，待 Job Object 指派完成后再恢复
    //    运行 —— 否则子进程可能在指派前就派生孙进程，逃脱 job 的覆盖。
    const bool want_isolation = opts.isolation.has_value();
    DWORD creation_flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (want_isolation) {
        creation_flags |= CREATE_SUSPENDED;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr,          // lpApplicationName（nullptr 表示从 cmdline 解析）
                        wcmdline.data(),  // lpCommandLine（可写缓冲区）
                        nullptr, nullptr,  // 进程/线程安全属性
                        TRUE,              // bInheritHandles
                        creation_flags,    // dwCreationFlags
                        nullptr,           // lpEnvironment（继承父进程）
                        wcwd.empty() ? nullptr : wcwd.c_str(), &si, &pi)) {
        DWORD err = GetLastError();
        return ResultV2<ExecOutput>::err(
            Error::Code::ResourceNotFound,
            "CreateProcessW failed for '" + cmd + "' (error " + std::to_string(err) + ")",
            "subprocess::exec");
    }
    HandleGuard g_process(pi.hProcess), g_thread(pi.hThread);

    // 3b. 进程级隔离：挂起 → 指派到 Job → 恢复运行（降级不阻断命令）
    IsolationOutcome isolation_outcome = IsolationOutcome::NotApplicable;
    JobGuard g_job;
    if (want_isolation) {
        isolation_outcome =
            apply_process_isolation(*opts.isolation, pi.hProcess, pi.hThread, &g_job.h);
    }

    // 4. 关闭父进程持有的写端，让子进程的 EOF 能传到读端
    CloseHandle(g_stdout_write.release());
    CloseHandle(g_stderr_write.release());

    // 5. 循环读取管道 + 检查超时/取消
    ExecOutput output;
    output.isolation = isolation_outcome;
    const auto start = std::chrono::steady_clock::now();
    bool stdout_open = true, stderr_open = true;

    while (stdout_open || stderr_open) {
        // 检查取消
        if (opts.is_cancelled && opts.is_cancelled()) {
            terminate_child(pi.hProcess, g_job.h);
            output.cancelled = true;
            break;
        }
        // 检查超时
        if (opts.timeout) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= *opts.timeout) {
                terminate_child(pi.hProcess, g_job.h);
                output.timed_out = true;
                break;
            }
        }

        bool got_data = false;
        // 读 stdout
        if (stdout_open) {
            if (output.stdout_text.size() >= opts.max_output_bytes) {
                // 缓冲区已满，标记截断，读弃数据以防止子进程管道阻塞
                output.stdout_truncated = true;
                std::string discard;
                if (read_pipe(stdout_read, discard, 65536)) {
                    got_data = true;
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) &&
                        available == 0) {
                        stdout_open = false;
                    }
                }
            } else {
                if (read_pipe(stdout_read, output.stdout_text, opts.max_output_bytes)) {
                    got_data = true;
                    // 读取后再次检查是否到达上限
                    if (output.stdout_text.size() >= opts.max_output_bytes) {
                        output.stdout_truncated = true;
                    }
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) &&
                        available == 0) {
                        stdout_open = false;
                    }
                }
            }
        }

        // 读 stderr
        if (stderr_open) {
            if (output.stderr_text.size() >= opts.max_output_bytes) {
                output.stderr_truncated = true;
                std::string discard;
                if (read_pipe(stderr_read, discard, 65536)) {
                    got_data = true;
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr) &&
                        available == 0) {
                        stderr_open = false;
                    }
                }
            } else {
                if (read_pipe(stderr_read, output.stderr_text, opts.max_output_bytes)) {
                    got_data = true;
                    if (output.stderr_text.size() >= opts.max_output_bytes) {
                        output.stderr_truncated = true;
                    }
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr) &&
                        available == 0) {
                        stderr_open = false;
                    }
                }
            }
        }

        // 无数据时短暂 sleep，避免 busy loop
        if (!got_data) {
            // 检查子进程是否已退出
            DWORD exit_code = 0;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                // 进程已退出，drain 剩余管道数据
                while (read_pipe(stdout_read, output.stdout_text, opts.max_output_bytes)) {
                }
                while (read_pipe(stderr_read, output.stderr_text, opts.max_output_bytes)) {
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // 6. 等待子进程完全退出
    if (!output.timed_out && !output.cancelled) {
        WaitForSingleObject(pi.hProcess, 5000);
    } else if (g_job.h != nullptr) {
        // TerminateJobObject 是异步的：等直接子进程真正死掉再取退出码，
        // 顺带保证 job 内其余进程（含孙进程）也已被清理。
        WaitForSingleObject(pi.hProcess, 3000);
    }

    // 7. 获取退出码
    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        output.exit_code = static_cast<int>(exit_code);
    }

    return ResultV2<ExecOutput>::ok(std::move(output));
}

#else   // !_WIN32
// ============================================================
// POSIX 实现
// ============================================================

/// 设置文件描述符为非阻塞
bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// 从非阻塞 fd 读取数据到 buf
/// @return 读取字节数（>0 成功）；0 表示 EOF 或错误（调用方应关闭管道）；
///         -1 表示暂无数据（EAGAIN/EWOULDBLOCK，应继续轮询）
ssize_t read_nonblocking(int fd, std::string& buf, size_t max_bytes) {
    if (buf.size() >= max_bytes) return 0;
    size_t to_read = std::min<size_t>(8192, max_bytes - buf.size());
    std::string chunk(to_read, '\0');
    ssize_t n = read(fd, chunk.data(), to_read);
    if (n > 0) {
        chunk.resize(static_cast<size_t>(n));
        buf += chunk;
        return n;
    }
    if (n == 0) return 0;  // EOF
    // EAGAIN/EWOULDBLOCK：暂无数据，继续轮询
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    // 其他错误（EIO/EBADF 等）：视为 EOF，调用方应关闭管道
    return 0;
}

ResultV2<ExecOutput> exec_posix(const std::string& cmd, const ExecOptions& opts) {
    // 1. 创建 stdout/stderr 管道
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) < 0) {
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
                                         "pipe(stdout) failed: " + std::string(strerror(errno)),
                                         "subprocess::exec");
    }
    if (pipe(stderr_pipe) < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
                                         "pipe(stderr) failed: " + std::string(strerror(errno)),
                                         "subprocess::exec");
    }

    // 2. fork
    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
                                         "fork() failed: " + std::string(strerror(errno)),
                                         "subprocess::exec");
    }

    if (pid == 0) {
        // ---- 子进程 ----
        // 重定向 stdout/stderr 到管道写端
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        // 切换工作目录
        if (!opts.cwd.empty()) {
            if (chdir(opts.cwd.c_str()) != 0) {
                _exit(127);
            }
        }

        // 重置信号处理（子进程不应继承父进程的信号 handler）
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        // #23 P1：建立独立进程组，使父进程可用 kill(-pid) 销毁整棵进程树
        // （bash -c "sleep 1000" 的 sleep 等子孙进程与 bash 同组，一并被清）。
        // 父子双侧都调用：子侧保证自身入组，父侧兜底竞态窗口（幂等，可忽略失败）。
        setpgid(0, 0);

        // 构建 argv
        std::vector<const char*> argv;
        argv.reserve(opts.args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto& arg : opts.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char* const*>(argv.data()));
        // execvp 失败（命令不存在等）
        _exit(127);
    }

    // ---- 父进程 ----
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    // #23 P1：竞态兜底 —— 与子进程的 setpgid(0,0) 幂等，确保 kill(-pid)
    //         在进入取消/超时分支前进程组一定已建立（失败可忽略）。
    setpgid(pid, pid);

    // 设置读端非阻塞
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    // 3. poll 循环读取管道 + 检查超时/取消
    ExecOutput output;
    const auto start = std::chrono::steady_clock::now();
    bool stdout_open = true, stderr_open = true;

    while (stdout_open || stderr_open) {
        // 检查取消
        if (opts.is_cancelled && opts.is_cancelled()) {
            kill(-pid, SIGTERM);  // 杀整个进程组（含 bash 的子孙进程）
            // 给 5s 升级到 SIGKILL
            int status = 0;
            for (int i = 0; i < 50; ++i) {  // 5s = 50 * 100ms
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid || w == -1) goto kill_done_cancelled;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
        kill_done_cancelled:
            output.cancelled = true;
            break;
        }

        // 检查超时
        if (opts.timeout) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= *opts.timeout) {
                kill(-pid, SIGTERM);  // 杀整个进程组（含 bash 的子孙进程）
                // 5s 升级 SIGKILL
                int status = 0;
                for (int i = 0; i < 50; ++i) {
                    pid_t w = waitpid(pid, &status, WNOHANG);
                    if (w == pid || w == -1) goto kill_done_timeout;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                kill(-pid, SIGKILL);
                waitpid(pid, &status, 0);
            kill_done_timeout:
                output.timed_out = true;
                break;
            }
        }

        // poll 管道
        struct pollfd fds[2];
        int nfds = 0;
        if (stdout_open) {
            fds[nfds].fd = stdout_pipe[0];
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (stderr_open) {
            fds[nfds].fd = stderr_pipe[0];
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (nfds == 0) break;

        // 100ms 超时让循环能定期检查 is_cancelled/timeout
        int poll_ret = poll(fds, nfds, 100);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 处理可读事件
        int fd_idx = 0;
        if (stdout_open) {
            if (fds[fd_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (output.stdout_text.size() >= opts.max_output_bytes) {
                    // 缓冲区已满，标记截断，读弃数据以防止子进程管道阻塞
                    output.stdout_truncated = true;
                    char discard[8192];
                    ssize_t n = read(stdout_pipe[0], discard, sizeof(discard));
                    if (n == 0) stdout_open = false;  // EOF
                } else {
                    ssize_t n =
                        read_nonblocking(stdout_pipe[0], output.stdout_text, opts.max_output_bytes);
                    if (n == 0)
                        stdout_open = false;  // EOF
                    else if (n > 0 && output.stdout_text.size() >= opts.max_output_bytes) {
                        output.stdout_truncated = true;
                    }
                }
            }
            ++fd_idx;
        }
        if (stderr_open) {
            if (fds[fd_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (output.stderr_text.size() >= opts.max_output_bytes) {
                    output.stderr_truncated = true;
                    char discard[8192];
                    ssize_t n = read(stderr_pipe[0], discard, sizeof(discard));
                    if (n == 0) stderr_open = false;
                } else {
                    ssize_t n =
                        read_nonblocking(stderr_pipe[0], output.stderr_text, opts.max_output_bytes);
                    if (n == 0)
                        stderr_open = false;
                    else if (n > 0 && output.stderr_text.size() >= opts.max_output_bytes) {
                        output.stderr_truncated = true;
                    }
                }
            }
        }
    }

    // 4. 等待子进程退出（如果还没被 kill）
    if (!output.timed_out && !output.cancelled) {
        int status = 0;
        if (waitpid(pid, &status, 0) > 0) {
            if (WIFEXITED(status)) {
                output.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                output.exit_code = -1;
            }
        }
    } else {
        output.exit_code = -1;
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    return ResultV2<ExecOutput>::ok(std::move(output));
}
#endif  // _WIN32

}  // anonymous namespace

ResultV2<ExecOutput> exec(const std::string& cmd, const ExecOptions& opts) {
#ifdef _WIN32
    auto result = exec_windows(cmd, opts);
#else
    auto result = exec_posix(cmd, opts);
#endif
    // 统一对子进程输出做 UTF-8 清洗，避免 GBK 等非 UTF-8 字节导致
    // 后续 nlohmann::json 序列化抛 type_error.316（如 Windows cmd 中文系统输出）
    if (result.is_ok()) {
        auto& out = result.value();
        out.stdout_text = sanitize_output_to_utf8(std::move(out.stdout_text));
        out.stderr_text = sanitize_output_to_utf8(std::move(out.stderr_text));
    }
    return result;
}

// ============================================================
// exec_interactive：交互式子进程（继承终端 stdio，不捕获输出）
// ============================================================

namespace {

#ifdef _WIN32
ResultV2<InteractiveExecResult> exec_interactive_windows(const std::string& cmd,
                                                         const std::vector<std::string>& args,
                                                         const std::string& cwd) {
    std::string cmdline = escape_arg(cmd);
    for (const auto& arg : args) {
        cmdline += ' ';
        cmdline += escape_arg(arg);
    }
    std::wstring wcmdline = utf8_to_wide(cmdline);
    std::wstring wcwd = utf8_to_wide(cwd);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // 不设置 STARTF_USESTDHANDLES → 子进程继承父进程控制台 stdio（nvim 需要交互式终端）
    // 不设置 CREATE_NO_WINDOW → 子进程复用父进程控制台窗口
    if (!CreateProcessW(nullptr, wcmdline.data(), nullptr, nullptr,
                        TRUE,  // bInheritHandles（继承标准句柄）
                        CREATE_UNICODE_ENVIRONMENT, nullptr, wcwd.empty() ? nullptr : wcwd.c_str(),
                        &si, &pi)) {
        DWORD err = GetLastError();
        return ResultV2<InteractiveExecResult>::err(
            Error::Code::ResourceNotFound,
            "CreateProcessW failed for '" + cmd + "' (error " + std::to_string(err) + ")",
            "subprocess::exec_interactive");
    }
    HandleGuard g_process(pi.hProcess), g_thread(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        return ResultV2<InteractiveExecResult>::ok(
            InteractiveExecResult{static_cast<int>(exit_code)});
    }
    return ResultV2<InteractiveExecResult>::err(
        Error::Code::InternalError, "GetExitCodeProcess failed", "subprocess::exec_interactive");
}
#else   // !_WIN32
ResultV2<InteractiveExecResult> exec_interactive_posix(const std::string& cmd,
                                                       const std::vector<std::string>& args,
                                                       const std::string& cwd) {
    // 子进程 → 父进程：execvp 失败时传递 errno，区分"命令不存在"与"退出码 127"
    int err_pipe[2] = {-1, -1};
    if (pipe(err_pipe) < 0) {
        return ResultV2<InteractiveExecResult>::err(
            Error::Code::InternalError, "pipe() failed: " + std::string(strerror(errno)),
            "subprocess::exec_interactive");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return ResultV2<InteractiveExecResult>::err(
            Error::Code::InternalError, "fork() failed: " + std::string(strerror(errno)),
            "subprocess::exec_interactive");
    }

    if (pid == 0) {
        // 子进程：不重定向 stdio（继承父进程终端），留在前台进程组（可接收终端信号）
        close(err_pipe[0]);  // 子进程只写
        if (!cwd.empty()) {
            if (chdir(cwd.c_str()) != 0) {
                const int e = errno;
                (void)!write(err_pipe[1], &e, sizeof(e));
                _exit(127);
            }
        }
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto& arg : args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char* const*>(argv.data()));
        // execvp 失败（命令不存在等）：把 errno 传给父进程
        const int e = errno;
        (void)!write(err_pipe[1], &e, sizeof(e));
        _exit(127);
    }

    close(err_pipe[1]);  // 父进程只读

    int status = 0;
    if (waitpid(pid, &status, 0) > 0) {
        if (WIFEXITED(status)) {
            const int code = WEXITSTATUS(status);
            if (code == 127) {
                // 子进程可能因 execvp/chdir 失败退出：读 errno 判断启动失败
                int e = 0;
                const ssize_t n = read(err_pipe[0], &e, sizeof(e));
                close(err_pipe[0]);
                if (n == static_cast<ssize_t>(sizeof(e))) {
                    return ResultV2<InteractiveExecResult>::err(
                        Error::Code::ResourceNotFound,
                        "execvp failed for '" + cmd + "': " + std::string(strerror(e)),
                        "subprocess::exec_interactive");
                }
                return ResultV2<InteractiveExecResult>::ok(InteractiveExecResult{code});
            }
            close(err_pipe[0]);
            return ResultV2<InteractiveExecResult>::ok(InteractiveExecResult{code});
        }
        close(err_pipe[0]);
        return ResultV2<InteractiveExecResult>::ok(InteractiveExecResult{-1});  // 被信号终止
    }
    close(err_pipe[0]);
    return ResultV2<InteractiveExecResult>::err(Error::Code::InternalError,
                                                "waitpid failed: " + std::string(strerror(errno)),
                                                "subprocess::exec_interactive");
}
#endif  // _WIN32

}  // anonymous namespace

ResultV2<InteractiveExecResult> exec_interactive(const std::string& cmd,
                                                 const std::vector<std::string>& args,
                                                 const std::string& cwd) {
#ifdef _WIN32
    return exec_interactive_windows(cmd, args, cwd);
#else
    return exec_interactive_posix(cmd, args, cwd);
#endif
}

}  // namespace agent::process
