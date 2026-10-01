/**
 * @file transport_posix.cpp
 * @brief POSIX AF_UNIX socket 传输实现（macOS / Linux）
 * @details 端点：$XDG_RUNTIME_DIR（Linux）或 $TMPDIR（macOS）或 /tmp，
 *          文件名 workx-island-<uid>-<pid>.sock。
 *          stop() 时 shutdown+close 使阻塞的 accept/recv 返回
 *          （Linux 已验证；macOS 见 close_and_wake 注释中的平台差异）。
 * @version 1.0.1
 * @date 2026-08
 */

#if defined(__unix__) || defined(__APPLE__)

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include <chrono>
#include <thread>

#include "island/ipc/itransport.h"

namespace island::ipc {

namespace {

/// @brief 关闭 fd 并**唤醒**阻塞在该 fd 上的 accept()/recv()。
/// @details POSIX 语义陷阱：::close() 只递减描述符引用计数，并**不会**唤醒另一线程中
///          已阻塞在 accept()/recv() 上的系统调用。也就是说"stop() 时 close 掉句柄，
///          阻塞调用自然会返回"这个直觉在 Linux 上不成立 —— 实际结果是服务端线程
///          永久挂起（#105：island 12 项用例在 CI 上全部 ctest Timeout）。
///          因此关闭前必须先 shutdown(fd, SHUT_RDWR)：
///            · Linux：监听 socket 上阻塞的 accept() 立即返回 -1/EINVAL，
///              连接 socket 上阻塞的 recv() 返回 0（EOF），线程得以退出、join 得以返回。
///            · macOS：对**监听** socket 调用 shutdown() 可能返回 ENOTCONN 且不唤醒
///              accept()。届时仍需把阻塞调用改成 poll() + 超时循环或 self-pipe
///              （POSIX 通用正解，见 #105 后续项）。CI 当前只覆盖 Linux。
/// @note 先把成员置 -1 再关 fd：若与 accept() 线程并发，可避免 double-close
///       （fd 号被复用后二次 close 会误关他人描述符，属难复现的隐蔽故障）。
/// @brief connect() 重试次数（覆盖服务端重 listen 的窗口，见 connect() 注释）
constexpr int kConnectAttempts = 6;
/// @brief connect() 重试间隔（总窗口 ≈ 6 × 100ms = 600ms，与 Windows 侧 3 × 100ms 同量级）
constexpr int kConnectRetryDelayMs = 100;

void close_and_wake(int& fd) {
    if (fd < 0) return;
    const int raw = fd;
    fd = -1;
    ::shutdown(raw, SHUT_RDWR);  // 返回值无意义：未连接/已关闭时失败亦无副作用
    ::close(raw);
}

class UnixSocketTransport final : public ITransport {
   public:
    ~UnixSocketTransport() override { close(); }

    bool listen(const std::string& endpoint) override {
        close();  // 支持重 listen（断连后重新创建实例）
        m_endpoint = endpoint;
        // 清理上次残留的 socket 文件（同端点路径，如未正常退出的旧进程）
        unlink(endpoint.c_str());
        m_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (m_listen_fd < 0) return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (endpoint.size() >= sizeof(addr.sun_path)) {
            close();
            return false;
        }
        std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }
        if (::listen(m_listen_fd, 1) != 0) {
            close();
            return false;
        }
        return true;
    }

    bool accept() override {
        if (m_listen_fd < 0) return false;
        const int fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (fd < 0) return false;
        // 关闭 listener（连接期间不再接受新客户端），但保留 socket 文件
        // （GUI 断线重连需要端点路径可 connect，文件在 close() 清理）
        close_and_wake(m_listen_fd);
        m_conn_fd = fd;
        return true;
    }

    bool connect(const std::string& endpoint) override {
        close();
        // 与 Windows 侧（transport_win32.cpp）对称的重试：服务端 accept_loop 在
        // 「上一条连接断开 → 重新 listen」之间存在窗口 —— socket 文件还在但**没有
        // 监听者**，此时 connect 返回 ECONNREFUSED；若正赶上 listen() 先 unlink 了
        // 旧文件、尚未 bind 新文件，则返回 ENOENT。
        // 断线重连的客户端几乎必然撞进这个窗口（server 要被内核唤醒、读 EOF、
        // 退出 handle_connection 再 listen），POSIX 侧此前没有重试 → Linux 上
        // 重连必失败（#105 的 hang 修完后才暴露出来）。
        for (int attempt = 0; attempt < kConnectAttempts; ++attempt) {
            const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return false;

            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            if (endpoint.size() >= sizeof(addr.sun_path)) {
                ::close(fd);
                return false;
            }
            std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);

            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                m_conn_fd = fd;
                return true;
            }
            const int err = errno;
            ::close(fd);
            // 只重试"服务端还没准备好"这一类；其余（权限、路径过长等）立即失败
            if (err != ECONNREFUSED && err != ENOENT) return false;
            if (attempt + 1 < kConnectAttempts) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kConnectRetryDelayMs));
            }
        }
        return false;
    }

    ssize_t read(std::span<std::byte> buf) override {
        if (m_conn_fd < 0) return -1;
        const ssize_t n = ::recv(m_conn_fd, buf.data(), buf.size(), 0);
        return n;  // 0 = 对端关闭（EOF），<0 = 错误
    }

    // 对端已关闭时 send() 触发 SIGPIPE，默认动作是杀死进程。
    // Linux 用 MSG_NOSIGNAL 改为返回 -1/EPIPE（macOS 需 SO_NOSIGPIPE）。
    ssize_t write(std::span<const std::byte> data) override {
        if (m_conn_fd < 0) return -1;
#ifdef MSG_NOSIGNAL
        constexpr int kSendFlags = MSG_NOSIGNAL;
#else
        constexpr int kSendFlags = 0;
#endif
        const char* p = reinterpret_cast<const char*>(data.data());
        size_t remaining = data.size();
        while (remaining > 0) {
            const ssize_t n = ::send(m_conn_fd, p, remaining, kSendFlags);
            if (n <= 0) return -1;
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(data.size());
    }

    void close() override {
        // 必须走 close_and_wake：只 ::close() 不会唤醒阻塞在 accept()/recv() 上的
        // 服务端线程，stop() 的 join() 会永久挂起（#105）。
        close_and_wake(m_conn_fd);
        close_and_wake(m_listen_fd);
        if (!m_endpoint.empty()) {
            unlink(m_endpoint.c_str());  // 清理 socket 文件
            m_endpoint.clear();
        }
    }

    bool is_connected() const override { return m_conn_fd >= 0; }

   private:
    int m_listen_fd = -1;
    int m_conn_fd = -1;
    std::string m_endpoint;
};

/// @brief 获取 socket 文件目录（XDG_RUNTIME_DIR > TMPDIR > /tmp）
std::string runtime_dir() {
    if (const char* xdg = std::getenv("XDG_RUNTIME_DIR"); xdg && *xdg) return xdg;
    if (const char* tmp = std::getenv("TMPDIR"); tmp && *tmp) return tmp;
    return "/tmp";
}

}  // namespace

std::string default_endpoint(uint32_t pid) {
    const long uid = static_cast<long>(getuid());
    return runtime_dir() + "/workx-island-" + std::to_string(uid) + "-" + std::to_string(pid) +
           ".sock";
}

std::unique_ptr<ITransport> create_listener() { return std::make_unique<UnixSocketTransport>(); }

std::unique_ptr<ITransport> create_connector() { return std::make_unique<UnixSocketTransport>(); }

}  // namespace island::ipc

#endif  // __unix__ || __APPLE__