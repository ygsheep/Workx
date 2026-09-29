/**
 * @file transport_win32.cpp
 * @brief Windows named pipe 传输实现（CreateNamedPipe / ConnectNamedPipe）
 * @details 单客户端实例（PIPE_NUM_INSTANCES=1）：一个 GUI 连一个 TUI。
 *          stop() 关闭句柄使阻塞的 ConnectNamedPipe/ReadFile 返回错误。
 * @version 1.0.1
 * @date 2026-08
 */

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "island/ipc/itransport.h"

namespace island::ipc {

namespace {

class NamedPipeTransport final : public ITransport {
   public:
    ~NamedPipeTransport() override { close(); }

    bool listen(const std::string& endpoint) override {
        close();  // 支持重 listen（断连后重新创建实例）
        const HANDLE h =
            CreateNamedPipeA(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1,  // 单客户端实例
                             65536, 65536, 0, nullptr);
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            m_endpoint = endpoint;
            m_handle = h;
        }
        return h != INVALID_HANDLE_VALUE;
    }

    bool accept() override {
        HANDLE h;
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            h = m_handle;  // 快照：阻塞调用期间 close() 可并发关闭句柄
        }
        if (h == INVALID_HANDLE_VALUE) return false;
        // OVERLAPPED 模式：使阻塞的 accept 可被另一线程 CancelIoEx 取消（stop() 场景）
        OVERLAPPED ov{};
        ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        const BOOL ok = ConnectNamedPipe(h, &ov);
        if (ok) {
            CloseHandle(ov.hEvent);
            return true;
        }
        const DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED) {
            CloseHandle(ov.hEvent);
            return true;  // 客户端在等待间隙抢先连接
        }
        if (err != ERROR_IO_PENDING) {
            CloseHandle(ov.hEvent);
            return false;
        }
        DWORD unused = 0;
        const BOOL done = GetOverlappedResult(h, &ov, &unused, TRUE);
        CloseHandle(ov.hEvent);
        return done;  // 被 CancelIoEx 取消（stop）或出错
    }

    bool connect(const std::string& endpoint) override {
        close();
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const HANDLE fd = CreateFileA(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                          nullptr, OPEN_EXISTING, 0, nullptr);
            if (fd != INVALID_HANDLE_VALUE) {
                h = fd;  // byte 模式无需 SetNamedPipeHandleState
                break;
            }
            const DWORD err = GetLastError();
            // 实例忙（已连接）或不存在（服务器 accept 循环重 listen 间隙）：
            // WaitNamedPipe 在实例不存在时会立即失败，需短暂退避后重试
            if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) break;
            if (WaitNamedPipeA(endpoint.c_str(), 5000)) continue;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            m_endpoint = endpoint;
            m_handle = h;
        }
        return h != INVALID_HANDLE_VALUE;
    }

    ssize_t read(std::span<std::byte> buf) override {
        HANDLE h;
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            h = m_handle;
        }
        if (h == INVALID_HANDLE_VALUE) return -1;
        // 句柄以 FILE_FLAG_OVERLAPPED 打开（accept 可取消），读写必须走
        // OVERLAPPED，禁止同步调用（未定义行为，会偶发返回 0/EOF）
        OVERLAPPED ov{};
        ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return -1;
        DWORD nread = 0;
        if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &nread, &ov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                CloseHandle(ov.hEvent);
                return -1;
            }
            if (!GetOverlappedResult(h, &ov, &nread, TRUE)) {
                CloseHandle(ov.hEvent);
                return -1;
            }
        }
        CloseHandle(ov.hEvent);
        return static_cast<ssize_t>(nread);
    }

    ssize_t write(std::span<const std::byte> data) override {
        HANDLE h;
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            h = m_handle;
        }
        if (h == INVALID_HANDLE_VALUE) return -1;
        const char* p = reinterpret_cast<const char*>(data.data());
        size_t remaining = data.size();
        while (remaining > 0) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 65536));
            DWORD written = 0;
            OVERLAPPED ov{};
            ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (!ov.hEvent) return -1;
            if (!WriteFile(h, p, chunk, &written, &ov)) {
                const DWORD err = GetLastError();
                if (err != ERROR_IO_PENDING) {
                    CloseHandle(ov.hEvent);
                    return -1;
                }
                if (!GetOverlappedResult(h, &ov, &written, TRUE)) {
                    CloseHandle(ov.hEvent);
                    return -1;
                }
            }
            CloseHandle(ov.hEvent);
            p += written;
            remaining -= written;
            if (written == 0) return -1;  // 防呆：零进度不再循环
        }
        return static_cast<ssize_t>(data.size());
    }

    void close() override {
        HANDLE h;
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            h = std::exchange(m_handle, INVALID_HANDLE_VALUE);
        }
        if (h != INVALID_HANDLE_VALUE) {
            // 先取消本句柄上挂起的 I/O（阻塞中的 accept/read），再关闭句柄
            CancelIoEx(h, nullptr);
            CloseHandle(h);
        }
    }

    bool is_connected() const override {
        std::lock_guard<std::mutex> lock(m_handle_mutex);
        return m_handle != INVALID_HANDLE_VALUE;
    }

   private:
    mutable std::mutex m_handle_mutex;
    HANDLE m_handle = INVALID_HANDLE_VALUE;
    std::string m_endpoint;
};

}  // namespace

std::string default_endpoint(uint32_t pid) {
    return "\\\\.\\pipe\\workx-island-" + std::to_string(pid);
}

std::unique_ptr<ITransport> create_listener() { return std::make_unique<NamedPipeTransport>(); }

std::unique_ptr<ITransport> create_connector() { return std::make_unique<NamedPipeTransport>(); }

}  // namespace island::ipc

#endif  // _WIN32