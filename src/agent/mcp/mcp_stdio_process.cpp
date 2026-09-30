/**
 * @file mcp_stdio_process.cpp
 * @brief 持久子进程（双向管道）实现
 * @details
 * Windows 实现：
 *   - CreatePipe 创建 stdin/stdout 管道（子进程持有继承端，父进程持另一端）
 *   - CreateProcessW + STARTF_USESTDHANDLES 启动子进程
 *   - 读线程 ReadFile 循环读取 stdout，按 '\n' 切分行
 *   - WriteFile 写 stdin；stop() 关闭写端 + TerminateProcess
 *
 * POSIX 实现：
 *   - pipe() 创建 stdin/stdout 管道
 *   - fork + execvp 启动子进程
 *   - 读线程 read() 循环读取 stdout，按 '\n' 切分行
 *   - write() 写 stdin；stop() 关闭写端 + kill
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_stdio_process.h"

#include <chrono>
#include <cstring>
#include <cwctype>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent::mcp {

namespace {

#ifdef _WIN32
/// RAII 包装 Windows HANDLE
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : h(handle) {}
    ~HandleGuard() {
        if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
    }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HANDLE release() {
        HANDLE tmp = h;
        h = INVALID_HANDLE_VALUE;
        return tmp;
    }
};

/// 创建匿名管道，一端可继承（子进程持有），另一端父进程持有且不可继承
bool create_pipe(HANDLE* parent_end, HANDLE* child_end, bool child_reads) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read_end = INVALID_HANDLE_VALUE, write_end = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) return false;
    // 父进程持有的端设为不可继承
    HANDLE parent_keep = child_reads ? write_end : read_end;
    HANDLE child_keep = child_reads ? read_end : write_end;
    if (!SetHandleInformation(parent_keep, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(read_end);
        CloseHandle(write_end);
        return false;
    }
    *parent_end = parent_keep;
    *child_end = child_keep;
    return true;
}

/// UTF-8 → UTF-16
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

/// 构建 UTF-16 环境块（"K=V\0K=V\0\0"）
/// @details 必须从父进程环境合并后覆盖自定义变量，否则子进程会丢失
///          PATH / SystemRoot 等关键变量（如 Python 无法初始化熵源而崩溃）。
std::wstring build_environment_block(const std::map<std::string, std::string>& env) {
    std::map<std::wstring, std::wstring> merged;

    // 读取父进程环境块
    LPWCH parent_block = GetEnvironmentStringsW();
    if (parent_block) {
        for (LPWCH p = parent_block; *p != L'\0';) {
            std::wstring entry(p);
            const size_t eq = entry.find(L'=');
            if (eq != std::wstring::npos) {
                merged[entry.substr(0, eq)] = entry.substr(eq + 1);
            }
            p += entry.size() + 1;
        }
        FreeEnvironmentStringsW(parent_block);
    }

    // 覆盖自定义变量
    for (const auto& [k, v] : env) {
        merged[utf8_to_wide(k)] = utf8_to_wide(v);
    }

    std::wstring block;
    for (const auto& [k, v] : merged) {
        block += k;
        block += L'=';
        block += v;
        block += L'\0';
    }
    block += L'\0';
    return block;
}

/// 参数是否需要引号（含空格/制表符/引号时需要）
bool needs_quotes(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '"') return true;
    }
    return false;
}

/// 转义单个参数（CommandLineToArgvW 规则）
/// @details 引号前的反斜杠需加倍（2n+1 个 \ 后跟 " 表示 n 个字面 \ + 字面 "）；
///          末尾反斜杠在引号内也需加倍，避免与闭合引号组合成转义引号。
std::string escape_arg(const std::string& arg) {
    if (!needs_quotes(arg)) return arg;
    std::string result;
    result.reserve(arg.size() + 4);
    result += '"';
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            result.append(backslashes * 2 + 1, '\\');
            result += '"';
            backslashes = 0;
            continue;
        }
        result.append(backslashes, '\\');
        backslashes = 0;
        result += c;
    }
    result.append(backslashes * 2, '\\');
    result += '"';
    return result;
}

/// 解析命令为完整路径（PATH 搜索 + 扩展名补全）
/// @param cmd 配置中的命令（如 "npx"）
/// @param out_full 输出解析后的完整路径（UTF-16）
/// @return true=解析成功；false=未找到
/// @details 裸命令名按 PATHEXT 常见扩展名依次搜索（npx → npx.cmd 等）；
///          跳过无扩展名命中（PATH 中可能存在同名 shell 脚本，CreateProcessW
///          无法执行）；含路径分隔符的命令直接使用（相对/绝对路径）。
bool resolve_command(const std::string& cmd, std::wstring& out_full) {
    const std::wstring wcmd = utf8_to_wide(cmd);
    if (cmd.find('/') != std::string::npos || cmd.find('\\') != std::string::npos) {
        out_full = wcmd;
        return true;
    }
    static const wchar_t* kExts[] = {L".exe", L".cmd", L".bat", L".com"};
    std::vector<wchar_t> buf(32768);
    for (const wchar_t* ext : kExts) {
        DWORD n = SearchPathW(nullptr, wcmd.c_str(), ext, static_cast<DWORD>(buf.size()),
                              buf.data(), nullptr);
        if (n > 0 && n < buf.size()) {
            out_full.assign(buf.data(), n);
            return true;
        }
    }
    return false;
}

/// 是否批处理脚本（.cmd/.bat；CreateProcessW 无法直接执行，需经 cmd.exe /c）
bool is_batch_script(const std::wstring& path) {
    auto ends_with_ci = [&path](const wchar_t* suffix) {
        const size_t sl = std::wcslen(suffix);
        if (path.size() < sl) return false;
        for (size_t i = 0; i < sl; ++i) {
            if (std::towlower(path[path.size() - sl + i]) != std::towlower(suffix[i])) return false;
        }
        return true;
    };
    return ends_with_ci(L".cmd") || ends_with_ci(L".bat");
}
#endif  // _WIN32

}  // anonymous namespace

McpStdioProcess::McpStdioProcess() = default;

McpStdioProcess::~McpStdioProcess() { stop(); }

ResultV2<void> McpStdioProcess::start(const std::string& cmd, const std::vector<std::string>& args,
                                      const std::map<std::string, std::string>& env) {
#ifdef _WIN32
    // 1. 解析命令为完整路径（PATH 搜索 + 扩展名补全，如 npx → npx.cmd）
    std::wstring resolved;
    if (!resolve_command(cmd, resolved)) {
        return ResultV2<void>::err(Error::Code::ResourceNotFound,
                                   "无法在 PATH 中找到命令 '" + cmd + "'",
                                   "McpStdioProcess::start");
    }

    // 2. 构建命令行：批处理脚本（.cmd/.bat）CreateProcessW 无法直接执行，需经 cmd.exe /c
    std::wstring app_name;
    std::wstring cmdline;
    if (is_batch_script(resolved)) {
        std::wstring comspec(MAX_PATH, L'\0');
        const DWORD cs =
            GetEnvironmentVariableW(L"ComSpec", comspec.data(), static_cast<DWORD>(comspec.size()));
        if (cs == 0 || cs >= comspec.size()) {
            app_name = L"cmd.exe";  // 兜底：依赖 PATH 解析
        } else {
            comspec.resize(cs);
            app_name = comspec;
        }
        // cmd.exe /c 引号规则：整条命令外层再包一对引号，避免首尾引号被剥离
        cmdline = L"/c \"\"" + resolved + L"\"";
        for (const auto& arg : args) {
            cmdline += L' ';
            cmdline += utf8_to_wide(escape_arg(arg));
        }
        cmdline += L"\"";
    } else {
        app_name = resolved;
        cmdline = utf8_to_wide(escape_arg(cmd));
        for (const auto& arg : args) {
            cmdline += L' ';
            cmdline += utf8_to_wide(escape_arg(arg));
        }
    }

    // 3. 创建管道：stdin（子进程读端继承）、stdout（子进程写端继承）
    HANDLE stdin_parent = INVALID_HANDLE_VALUE, stdin_child = INVALID_HANDLE_VALUE;
    HANDLE stdout_parent = INVALID_HANDLE_VALUE, stdout_child = INVALID_HANDLE_VALUE;
    if (!create_pipe(&stdin_parent, &stdin_child, /*child_reads=*/true)) {
        return ResultV2<void>::err(Error::Code::InternalError, "CreatePipe(stdin) failed",
                                   "McpStdioProcess::start");
    }
    HandleGuard g_stdin_parent(stdin_parent), g_stdin_child(stdin_child);
    if (!create_pipe(&stdout_parent, &stdout_child, /*child_reads=*/false)) {
        return ResultV2<void>::err(Error::Code::InternalError, "CreatePipe(stdout) failed",
                                   "McpStdioProcess::start");
    }
    HandleGuard g_stdout_parent(stdout_parent), g_stdout_child(stdout_child);

    // 4. 启动子进程
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_child;
    si.hStdOutput = stdout_child;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);  // stderr 透传（调试可见）
    PROCESS_INFORMATION pi{};

    std::wstring env_block = build_environment_block(env);
    if (!CreateProcessW(app_name.c_str(), cmdline.data(), nullptr, nullptr,
                        TRUE,  // bInheritHandles（继承管道句柄）
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        env_block.empty() ? nullptr : env_block.data(), nullptr, &si, &pi)) {
        DWORD err = GetLastError();
        return ResultV2<void>::err(
            Error::Code::ResourceNotFound,
            "CreateProcessW failed for '" + cmd + "' (error " + std::to_string(err) + ")",
            "McpStdioProcess::start");
    }
    HandleGuard g_process(pi.hProcess), g_thread(pi.hThread);

    // 5. 保存父进程持有的端，释放子进程持有的端
    m_h_process = pi.hProcess;
    m_h_stdin_write = stdin_parent;
    m_h_stdout_read = stdout_parent;
    g_process.release();
    g_stdin_parent.release();
    g_stdout_parent.release();
    // 子进程持有的端由 HandleGuard 关闭（父进程侧副本）

    // 6. 启动读线程
    m_stopped = false;
    m_eof = false;
    m_reader = std::thread(&McpStdioProcess::reader_thread_main, this);
    return ResultV2<void>::ok();
#else
    // POSIX
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        if (stdin_pipe[0] >= 0) {
            close(stdin_pipe[0]);
            close(stdin_pipe[1]);
        }
        if (stdout_pipe[0] >= 0) {
            close(stdout_pipe[0]);
            close(stdout_pipe[1]);
        }
        return ResultV2<void>::err(Error::Code::InternalError,
                                   "pipe() failed: " + std::string(strerror(errno)),
                                   "McpStdioProcess::start");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return ResultV2<void>::err(Error::Code::InternalError,
                                   "fork() failed: " + std::string(strerror(errno)),
                                   "McpStdioProcess::start");
    }

    if (pid == 0) {
        // 子进程
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);

        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        setpgid(0, 0);

        // 设置额外环境变量
        for (const auto& [k, v] : env) {
            setenv(k.c_str(), v.c_str(), 1);
        }

        std::vector<const char*> argv;
        argv.reserve(args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto& arg : args) argv.push_back(arg.c_str());
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char* const*>(argv.data()));
        _exit(127);
    }

    // 父进程
    close(stdin_pipe[0]);
    close(stdout_pipe[1]);
    setpgid(pid, pid);

    m_pid = pid;
    m_stdin_fd = stdin_pipe[1];
    m_stdout_fd = stdout_pipe[0];

    m_stopped = false;
    m_eof = false;
    m_reader = std::thread(&McpStdioProcess::reader_thread_main, this);
    return ResultV2<void>::ok();
#endif
}

void McpStdioProcess::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stopped) return;
        m_stopped = true;
    }

    // 关闭 stdin 写端，让子进程读到 EOF（stdio server 会因此退出）
#ifdef _WIN32
    if (m_h_stdin_write) {
        CloseHandle(static_cast<HANDLE>(m_h_stdin_write));
        m_h_stdin_write = nullptr;
    }
#else
    if (m_stdin_fd >= 0) {
        close(m_stdin_fd);
        m_stdin_fd = -1;
    }
#endif

    // 先终止进程再 join 读线程：若子进程不读 stdin（如 HTTP server），
    // 关闭 stdin 不会让其退出，stdout 写端仍打开，读线程阻塞在 ReadFile/read，
    // 此时 join 会永久死锁。必须先等进程退出（或强杀）关闭写端，读线程才会返回。
#ifdef _WIN32
    if (m_h_process) {
        HANDLE h = static_cast<HANDLE>(m_h_process);
        if (WaitForSingleObject(h, 1000) != WAIT_OBJECT_0) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 1000);
        }
        CloseHandle(h);
        m_h_process = nullptr;
    }
#else
    if (m_pid > 0) {
        int status = 0;
        pid_t w = waitpid(m_pid, &status, WNOHANG);
        if (w == 0) {
            kill(-m_pid, SIGTERM);
            for (int i = 0; i < 20; ++i) {  // 2s
                w = waitpid(m_pid, &status, WNOHANG);
                if (w == m_pid || w == -1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (w == 0) {
                kill(-m_pid, SIGKILL);
                waitpid(m_pid, &status, 0);
            }
        }
        m_pid = -1;
    }
#endif

    // 进程已退出/被杀 → stdout 写端已关闭 → 读线程 ReadFile/read 返回 → join 可返回
    if (m_reader.joinable()) {
        m_reader.join();
    }

#ifdef _WIN32
    if (m_h_stdout_read) {
        CloseHandle(static_cast<HANDLE>(m_h_stdout_read));
        m_h_stdout_read = nullptr;
    }
#else
    if (m_stdout_fd >= 0) {
        close(m_stdout_fd);
        m_stdout_fd = -1;
    }
#endif
}

ResultV2<void> McpStdioProcess::write_line(const std::string& line) {
#ifdef _WIN32
    if (!m_h_stdin_write) {
        return ResultV2<void>::err(Error::Code::NetworkDisconnected, "MCP stdio 管道已关闭",
                                   "McpStdioProcess::write_line");
    }
    std::string data = line + "\n";
    DWORD written = 0;
    if (!WriteFile(static_cast<HANDLE>(m_h_stdin_write), data.data(),
                   static_cast<DWORD>(data.size()), &written, nullptr)) {
        return ResultV2<void>::err(Error::Code::NetworkDisconnected, "MCP stdio 写入失败",
                                   "McpStdioProcess::write_line");
    }
    return ResultV2<void>::ok();
#else
    if (m_stdin_fd < 0) {
        return ResultV2<void>::err(Error::Code::NetworkDisconnected, "MCP stdio 管道已关闭",
                                   "McpStdioProcess::write_line");
    }
    std::string data = line + "\n";
    ssize_t n = write(m_stdin_fd, data.data(), data.size());
    if (n < 0) {
        return ResultV2<void>::err(Error::Code::NetworkDisconnected,
                                   "MCP stdio 写入失败: " + std::string(strerror(errno)),
                                   "McpStdioProcess::write_line");
    }
    return ResultV2<void>::ok();
#endif
}

ResultV2<std::string> McpStdioProcess::read_line(int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_lines.empty()) {
        std::string line = std::move(m_lines.front());
        m_lines.pop_front();
        return ResultV2<std::string>::ok(std::move(line));
    }
    if (m_eof) {
        return ResultV2<std::string>::err(Error::Code::NetworkDisconnected, "MCP stdio 已 EOF",
                                          "McpStdioProcess::read_line");
    }

    if (timeout_ms <= 0) {
        m_cv.wait(lock, [this] { return !m_lines.empty() || m_eof; });
    } else {
        m_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [this] { return !m_lines.empty() || m_eof; });
    }

    if (!m_lines.empty()) {
        std::string line = std::move(m_lines.front());
        m_lines.pop_front();
        return ResultV2<std::string>::ok(std::move(line));
    }
    if (m_eof) {
        return ResultV2<std::string>::err(Error::Code::NetworkDisconnected, "MCP stdio 已 EOF",
                                          "McpStdioProcess::read_line");
    }
    return ResultV2<std::string>::err(Error::Code::NetworkTimeout,
                                      "MCP stdio 读取超时 (" + std::to_string(timeout_ms) + "ms)",
                                      "McpStdioProcess::read_line");
}

bool McpStdioProcess::is_alive() const {
#ifdef _WIN32
    if (!m_h_process) return false;
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(m_h_process), &exit_code)) return false;
    return exit_code == STILL_ACTIVE;
#else
    if (m_pid <= 0) return false;
    int status = 0;
    pid_t w = waitpid(m_pid, &status, WNOHANG);
    if (w == 0) return true;
    if (w == m_pid) return false;
    return true;
#endif
}

void McpStdioProcess::push_line(std::string line) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_lines.push_back(std::move(line));
    m_cv.notify_one();
}

void McpStdioProcess::reader_thread_main() {
    char buf[8192];
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(m_h_stdout_read);
    while (!m_stopped) {
        DWORD n = 0;
        if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        // 按行切分
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                m_buffer.append(buf + start, i - start);
                if (!m_buffer.empty() && m_buffer.back() == '\r') {
                    m_buffer.pop_back();
                }
                push_line(std::move(m_buffer));
                m_buffer.clear();
                start = i + 1;
            }
        }
        if (start < n) {
            m_buffer.append(buf + start, n - start);
        }
    }
#else
    while (!m_stopped) {
        ssize_t n = read(m_stdout_fd, buf, sizeof(buf));
        if (n <= 0) break;
        size_t start = 0;
        for (size_t i = 0; i < static_cast<size_t>(n); ++i) {
            if (buf[i] == '\n') {
                m_buffer.append(buf + start, i - start);
                if (!m_buffer.empty() && m_buffer.back() == '\r') {
                    m_buffer.pop_back();
                }
                push_line(std::move(m_buffer));
                m_buffer.clear();
                start = i + 1;
            }
        }
        if (start < static_cast<size_t>(n)) {
            m_buffer.append(buf + start, static_cast<size_t>(n) - start);
        }
    }
#endif
    // EOF：把残余缓冲作为最后一行推送
    if (!m_buffer.empty()) {
        push_line(std::move(m_buffer));
        m_buffer.clear();
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_eof = true;
    }
    m_cv.notify_all();
}

}  // namespace agent::mcp
