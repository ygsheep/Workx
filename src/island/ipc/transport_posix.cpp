/**
 * @file transport_posix.cpp
 * @brief POSIX AF_UNIX socket 传输实现（macOS / Linux）
 * @details 端点：$XDG_RUNTIME_DIR（Linux）或 $TMPDIR（macOS）或 /tmp，
 *          文件名 workx-island-<uid>-<pid>.sock。
 *          监听与连接是两个实例：accept_connection() 返回独立的连接实例，
 *          监听 fd 保持开放直至 close()，服务期间的新客户端进 backlog 排队
 *          （#118）。stop() 时 shutdown+close 使阻塞的 accept/recv 返回
 *          （Linux 已验证；macOS 见 close_and_wake 注释中的平台差异）。
 * @version 1.1.0
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

// connect() 重试次数（覆盖服务端尚未 listen 的启动窗口，见 connect() 注释）
constexpr int kConnectAttempts = 6;
// connect() 重试间隔（总窗口 ≈ 6 × 100ms = 600ms，与 Windows 侧 3 × 100ms 同量级）
constexpr int kConnectRetryDelayMs = 100;
// listen() 的 backlog：连接期间监听 fd 保持开放，重连客户端在此排队（#118）。
// 取 4 而非 1：即便服务端偶发调度抖动，多个排队客户端也不会被内核丢弃。
constexpr int kListenBacklog = 4;

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
void close_and_wake(int& fd) {
    if (fd < 0) return;
    const int raw = fd;
    fd = -1;
    ::shutdown(raw, SHUT_RDWR);  // 返回值无意义：未连接/已关闭时失败亦无副作用
    ::close(raw);
}

/// @brief 关掉 fd 上的 SIGPIPE 投递（macOS 专用）。
/// @details macOS 没有 MSG_NOSIGNAL（同名宏在部分 SDK 存在但不生效），改用
///          SO_NOSIGPIPE 让本模块的写路径**自洽**：对端关闭时 send() 返回
///          -1/EPIPE，而不是用默认动作杀死进程。
///          此前 macOS 分支等价于"隐式指望别处（MCP stdio 的 start()）已经设过
///          signal(SIGPIPE, SIG_IGN)"——跨模块的隐式依赖既难发现也难保证顺序，
///          故在此显式设置，两条路径各自自洽。
void disable_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

class UnixSocketTransport final : public ITransport {
   public:
    /// @brief 监听角色：无 fd，等 listen() 创建
    UnixSocketTransport() = default;

    /// @brief 连接角色：接管一个已建立的连接 fd（accept_connection() 的返回值）
    /// @note 该实例**不持有监听 fd、也不知道端点路径**，故析构只关连接、
    ///       不会 unlink 端点文件 —— 监听端点由创建它的实例负责。
    explicit UnixSocketTransport(int conn_fd) : m_conn_fd(conn_fd) {}

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
        if (::listen(m_listen_fd, kListenBacklog) != 0) {
            close();
            return false;
        }
        return true;
    }

    std::unique_ptr<ITransport> accept_connection() override {
        if (m_listen_fd < 0) return nullptr;
        const int fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (fd < 0) return nullptr;
        disable_sigpipe(fd);
        // 关键（#118）：**不关闭** m_listen_fd。监听 fd 保持开放，服务当前连接
        // 期间到来的客户端进 backlog 排队；连接实例只持有连接 fd，其析构不会
        // 关闭监听 fd，也不会 unlink 端点文件。
        return std::make_unique<UnixSocketTransport>(fd);
    }

    bool connect(const std::string& endpoint) override {
        close();
        // 与 Windows 侧（transport_win32.cpp）对称的重试：覆盖服务端**尚未
        // listen** 的启动窗口 —— socket 文件已建但还没有监听者时 connect
        // 返回 ECONNREFUSED。
        // 注：#118 之后监听 fd 全程开放，「上一条连接断开 → 重建监听」那个窗口
        // 已不存在，这里的重试只剩启动竞态这一处用途（客户端抢在 listen 前连）。
        //
        // 只对 ECONNREFUSED 重试：**不重试 ENOENT**。ENOENT 意为"端点路径不存在"，
        // 对「从未存在的端点」（test_ipc_transport.cpp 有用例专门测这个）而言重试
        // 纯属白等 500ms —— 实测该用例耗时从 0.00s 级涨到 0.50s。
        // （"listen 先 unlink 旧文件、尚未 bind 新文件"那个窗口只对**已知端点的
        //   重连**成立；要覆盖它就该让调用方显式表达"这是重连"，而不是在所有
        //   场景下一刀切地等 600ms。本期不做，见 #105 后续项。）
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
                disable_sigpipe(fd);
                m_conn_fd = fd;
                return true;
            }
            const int err = errno;
            ::close(fd);
            // 只重试"服务端还没准备好"这一类；其余（端点不存在、权限、路径过长）立即失败
            // 注：端点存在且有监听者时 connect 会成功并进 backlog，不会走到这里
            if (err != ECONNREFUSED) return false;
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

    // 对端已关闭时 send() 触发 SIGPIPE，默认动作是杀死进程：
    // Linux 用 MSG_NOSIGNAL、macOS 用建连时的 SO_NOSIGPIPE，均改为返回 -1/EPIPE。
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