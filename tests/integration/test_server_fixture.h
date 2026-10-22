/**
 * @file test_server_fixture.h
 * @brief 集成测试 Python 服务器自动启停 fixture
 * @details RAII 启动 tests/integration/fixtures/test_server.py，
 *          解析 stdout 中的 "TEST_SERVER_PORT=<port>" 获取端口，
 *          构造 base_url；析构时关闭子进程。
 *          若环境变量 LM_STUDIO_BASE_URL 已设置，则跳过自动启动，
 *          直接使用 LM Studio（向后兼容手动测试场景）。
 */

#pragma once

#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#endif

#include "agent/api/remote/http_client.h"

namespace agent::test {

/// @brief 自动启停 Python 测试服务器
/// @details X-2 修复：替代手动启动 LM Studio 的流程，让集成测试可独立运行
class AutoTestServer {
   public:
    AutoTestServer() {
        // 优先使用 LM Studio（若用户已显式设置环境变量）
        const char* env_url = std::getenv("LM_STUDIO_BASE_URL");
        if (env_url && env_url[0] != '\0') {
            m_base_url = env_url;
            while (!m_base_url.empty() && m_base_url.back() == '/') m_base_url.pop_back();
            m_uses_lm_studio = true;
            return;
        }

        // 否则自动启动 Python 测试服务器
        start_python_server();
    }

    ~AutoTestServer() { stop_python_server(); }

    AutoTestServer(const AutoTestServer&) = delete;
    AutoTestServer& operator=(const AutoTestServer&) = delete;

    /// @brief 获取测试服务器 base URL（不含末尾斜杠）
    const std::string& base_url() const { return m_base_url; }

    /// @brief 是否使用 LM Studio（true）或本地 Python 服务器（false）
    bool uses_lm_studio() const { return m_uses_lm_studio; }

    /// @brief 服务器是否就绪
    bool is_ready() const { return !m_base_url.empty(); }

   private:
    void start_python_server();
    void stop_python_server();

#ifdef _WIN32
    void start_python_server_win32();
    void stop_python_server_win32();
    HANDLE m_process_handle = nullptr;
    HANDLE m_thread_handle = nullptr;
#else
    void start_python_server_posix();
    void stop_python_server_posix();
    pid_t m_pid = -1;
#endif

    static std::string resolve_script_path() {
#ifdef WORKX_TEST_SERVER_SCRIPT
        return WORKX_TEST_SERVER_SCRIPT;
#else
        return "tests/integration/fixtures/test_server.py";
#endif
    }

    std::string m_base_url;
    std::string m_script_path = resolve_script_path();
    bool m_uses_lm_studio = false;
};

// ============================================================
// 平台相关实现
// ============================================================

inline void AutoTestServer::start_python_server() {
#ifdef _WIN32
    start_python_server_win32();
#else
    start_python_server_posix();
#endif
}

inline void AutoTestServer::stop_python_server() {
#ifdef _WIN32
    stop_python_server_win32();
#else
    stop_python_server_posix();
#endif
}

#ifdef _WIN32
inline void AutoTestServer::start_python_server_win32() {
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE pipe_read = nullptr, pipe_write = nullptr;
    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0)) return;
    SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "python \"" + m_script_path + "\"";

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = pipe_write;
    si.hStdError = pipe_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, const_cast<LPSTR>(cmd.c_str()), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return;
    }

    CloseHandle(pipe_write);
    m_process_handle = pi.hProcess;
    m_thread_handle = pi.hThread;

    // 读取 stdout 直到拿到 TEST_SERVER_PORT=（最多 5 秒）
    char buf[256];
    std::string accumulated;
    DWORD bytes_read = 0;
    for (int i = 0; i < 50; ++i) {
        while (PeekNamedPipe(pipe_read, nullptr, 0, nullptr, &bytes_read, nullptr) &&
               bytes_read > 0) {
            DWORD got = 0;
            if (!ReadFile(pipe_read, buf, sizeof(buf) - 1, &got, nullptr) || got == 0) break;
            buf[got] = '\0';
            accumulated += buf;
            auto pos = accumulated.find("TEST_SERVER_PORT=");
            if (pos != std::string::npos) {
                auto start = pos + std::string("TEST_SERVER_PORT=").size();
                auto end = accumulated.find_first_of("\r\n", start);
                m_base_url = "http://127.0.0.1:" + accumulated.substr(start, end - start);
                CloseHandle(pipe_read);
                return;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CloseHandle(pipe_read);
}

inline void AutoTestServer::stop_python_server_win32() {
    if (m_process_handle) {
        TerminateProcess(m_process_handle, 0);
        WaitForSingleObject(m_process_handle, 2000);
        CloseHandle(m_process_handle);
        m_process_handle = nullptr;
    }
    if (m_thread_handle) {
        CloseHandle(m_thread_handle);
        m_thread_handle = nullptr;
    }
}
#else
inline void AutoTestServer::start_python_server_posix() {
    // 简化实现：通过 pipe 获取子进程输出
    int pipefd[2];
    if (pipe(pipefd) != 0) return;

    m_pid = fork();
    if (m_pid == 0) {
        // 子进程：重定向 stdout 到 pipe 写端
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("python3", "python3", m_script_path.c_str(), nullptr);
        _exit(127);
    } else if (m_pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return;
    }

    // 父进程：读取子进程输出，获取 TEST_SERVER_PORT=
    close(pipefd[1]);
    char buf[256];
    std::string accumulated;
    for (int i = 0; i < 50; ++i) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            accumulated += buf;
            auto pos = accumulated.find("TEST_SERVER_PORT=");
            if (pos != std::string::npos) {
                auto start = pos + std::string("TEST_SERVER_PORT=").size();
                auto end = accumulated.find_first_of("\r\n", start);
                m_base_url = "http://127.0.0.1:" + accumulated.substr(start, end - start);
                break;
            }
        } else if (n == 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    close(pipefd[0]);
}

inline void AutoTestServer::stop_python_server_posix() {
    if (m_pid > 0) {
        kill(m_pid, SIGTERM);
        int status = 0;
        waitpid(m_pid, &status, 0);
        m_pid = -1;
    }
}
#endif

}  // namespace agent::test
