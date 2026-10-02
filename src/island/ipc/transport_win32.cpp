/**
 * @file transport_win32.cpp
 * @brief Windows named pipe 传输实现（CreateNamedPipe / ConnectNamedPipe）
 * @details 监听与连接是两个实例：accept_connection() 返回独立的连接实例，
 *          本实例随即另建监听实例，使端点在服务期间**保持可接入**（#118 的
 *          POSIX 对称实现）。实例上限 2 = 一个服务当前连接 + 一个等待接入。
 *          同一时刻仍只有一个 GUI 被服务（服务端串行 accept）。
 *          stop() 关闭句柄使阻塞的 ConnectNamedPipe/ReadFile 返回错误。
 * @version 1.1.0
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
    /// @brief 监听角色：无句柄，等 listen() 创建
    NamedPipeTransport() = default;

    /// @brief 连接角色：接管一个已建立连接的管道句柄（accept_connection() 的返回值）
    explicit NamedPipeTransport(HANDLE conn) : m_handle(conn) {}

    ~NamedPipeTransport() override { close(); }

    bool listen(const std::string& endpoint) override { return open_listen_instance(endpoint); }

    std::unique_ptr<ITransport> accept_connection() override {
        HANDLE h;
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            h = m_handle;  // 快照：阻塞调用期间 close() 可并发关闭句柄
        }
        if (h == INVALID_HANDLE_VALUE) return nullptr;
        // OVERLAPPED 模式：使阻塞的 accept 可被另一线程 CancelIoEx 取消（stop() 场景）
        OVERLAPPED ov{};
        ov.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!ov.hEvent) return nullptr;
        bool connected = false;
        const BOOL ok = ConnectNamedPipe(h, &ov);
        if (ok) {
            connected = true;
        } else {
            const DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED) {
                connected = true;  // 客户端在等待间隙抢先连接
            } else if (err == ERROR_IO_PENDING) {
                DWORD unused = 0;
                connected = GetOverlappedResult(h, &ov, &unused, TRUE) != FALSE;
                // 被 CancelIoEx 取消（stop）或出错 → connected 为 false
            }
        }
        CloseHandle(ov.hEvent);
        if (!connected) return nullptr;

        // #118 对称实现：该句柄现在是连接，摘出去交给独立实例；本实例随即另建
        // 监听实例，使端点在**服务当前连接期间**仍可接入 —— 新客户端在管道上
        // 等待 ConnectNamedPipe，而不是拿到 ERROR_FILE_NOT_FOUND 去忙等。
        std::string endpoint;
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            if (m_handle == h) m_handle = INVALID_HANDLE_VALUE;  // 摘出，避免被 close() 关掉
            endpoint = m_endpoint;
        }
        open_listen_instance(endpoint);
        return std::make_unique<NamedPipeTransport>(h);
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
    /// @brief 创建监听实例并接管到 m_handle（先关掉旧句柄）
    bool open_listen_instance(const std::string& endpoint) {
        close();  // 支持重建（旧句柄随进程/本实例回收）
        // kMaxInstances = 2：一个实例正服务当前连接，另一个等待下一个客户端接入。
        // 上限不能再低 —— 否则服务期间无法保有监听实例，#118 的窗口会重现。
        const HANDLE h =
            CreateNamedPipeA(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, kMaxInstances, 65536,
                             65536, 0, nullptr);
        {
            std::lock_guard<std::mutex> lock(m_handle_mutex);
            m_endpoint = endpoint;
            m_handle = h;
        }
        return h != INVALID_HANDLE_VALUE;
    }

    /// @brief 实例上限：1 个服务中 + 1 个待接入（见 open_listen_instance 注释）
    static constexpr DWORD kMaxInstances = 2;

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