/**
 * @file transport_posix.cpp
 * @brief POSIX AF_UNIX socket 传输实现（macOS / Linux）
 * @details 端点：$XDG_RUNTIME_DIR（Linux）或 $TMPDIR（macOS）或 /tmp，
 *          文件名 workx-island-<uid>-<pid>.sock。
 *          stop() 时 shutdown+close 使阻塞的 accept/recv 返回。
 * @version 1.0.0
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

#include "island/ipc/itransport.h"

namespace island::ipc {

namespace {

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
        ::close(m_listen_fd);
        m_listen_fd = -1;
        m_conn_fd = fd;
        return true;
    }

    bool connect(const std::string& endpoint) override {
        close();
        const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (endpoint.size() >= sizeof(addr.sun_path)) {
            ::close(fd);
            return false;
        }
        std::strncpy(addr.sun_path, endpoint.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return false;
        }
        m_conn_fd = fd;
        return true;
    }

    ssize_t read(std::span<std::byte> buf) override {
        if (m_conn_fd < 0) return -1;
        const ssize_t n = ::recv(m_conn_fd, buf.data(), buf.size(), 0);
        return n;  // 0 = 对端关闭（EOF），<0 = 错误
    }

    ssize_t write(std::span<const std::byte> data) override {
        if (m_conn_fd < 0) return -1;
        const char* p = reinterpret_cast<const char*>(data.data());
        size_t remaining = data.size();
        while (remaining > 0) {
            const ssize_t n = ::send(m_conn_fd, p, remaining, 0);
            if (n <= 0) return -1;
            p += n;
            remaining -= static_cast<size_t>(n);
        }
        return static_cast<ssize_t>(data.size());
    }

    void close() override {
        if (m_conn_fd >= 0) {
            ::close(m_conn_fd);
            m_conn_fd = -1;
        }
        if (m_listen_fd >= 0) {
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
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