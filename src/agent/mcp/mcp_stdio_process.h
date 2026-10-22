/**
 * @file mcp_stdio_process.h
 * @brief 持久子进程（双向管道）— MCP stdio 传输的进程管理（Issue #27）
 * @details MCP stdio server 需要长驻子进程 + 双向管道：
 *          - stdin：父进程写入 JSON-RPC 请求（按行）
 *          - stdout：子进程输出 JSON-RPC 响应（按行）
 *          现有 subprocess::exec() 是一次性同步执行，不满足需求，故独立实现。
 *
 *          跨平台实现：
 *          - Windows: CreateProcessW + 双向匿名管道 + 读线程
 *          - POSIX:   fork + execvp + pipe + 读线程
 *
 *          读线程持续读取 stdout 字节流，按 '\n' 切分为行放入队列；
 *          read_line() 从队列取行（带超时，condition_variable 等待）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/utils/result_v2.h"

namespace agent::mcp {

/// @brief 持久子进程：启动后保持运行，支持 stdin 写 / stdout 行读
class McpStdioProcess {
   public:
    McpStdioProcess();
    ~McpStdioProcess();

    McpStdioProcess(const McpStdioProcess&) = delete;
    McpStdioProcess& operator=(const McpStdioProcess&) = delete;

    /// @brief 启动子进程
    /// @param cmd 可执行文件路径（绝对路径或 PATH 中可找到的命令名）
    /// @param args 命令行参数（不含命令本身）
    /// @param env 额外环境变量（追加到父进程环境）
    /// @return ok: 已启动；err: 启动失败（命令不存在、管道创建失败）
    ResultV2<void> start(const std::string& cmd, const std::vector<std::string>& args,
                         const std::map<std::string, std::string>& env);

    /// @brief 终止子进程（关闭 stdin + 终止进程，幂等）
    void stop();

    /// @brief 写入一行（自动追加 '\n'）
    /// @return ok: 已写入；err: 管道已关闭/进程已退出
    ResultV2<void> write_line(const std::string& line);

    /// @brief 读取一行（阻塞，最多等待 timeout_ms）
    /// @return ok: 读到一行（不含 '\n'）；err: 超时（NetworkTimeout）/ EOF（NetworkDisconnected）
    ResultV2<std::string> read_line(int timeout_ms);

    /// @brief 进程是否存活
    bool is_alive() const;

   private:
    void reader_thread_main();
    void push_line(std::string line);

#ifdef _WIN32
    void* m_h_process = nullptr;      // HANDLE
    void* m_h_stdin_write = nullptr;  // HANDLE
    void* m_h_stdout_read = nullptr;  // HANDLE
#else
    int m_pid = -1;
    int m_stdin_fd = -1;
    int m_stdout_fd = -1;
#endif

    std::thread m_reader;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<std::string> m_lines;
    std::string m_buffer;    ///< 读线程的行缓冲（跨 read 调用保留）
    bool m_eof = false;      ///< stdout 已 EOF
    bool m_stopped = false;  ///< stop() 已调用
};

}  // namespace agent::mcp
