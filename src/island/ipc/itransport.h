/**
 * @file itransport.h
 * @brief Island IPC 传输层抽象（平台无关）
 * @details 阻塞 I/O + 线程模型（用户确认，替代设计文档的 asio）：
 *          - Windows:  named pipe  \\.\pipe\workx-island-<pid>
 *          - POSIX:    unix socket $XDG_RUNTIME_DIR (或 $TMPDIR / /tmp)
 *          服务端流程：listen → accept(阻塞) → 服务连接 → close → 重新 listen。
 *          stop() 时 close() 使阻塞的 accept/read 返回失败，线程据此退出。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

#ifdef _WIN32
#include <BaseTsd.h>
using ssize_t = SSIZE_T;  // MSVC 无 POSIX ssize_t；SSIZE_T = ptrdiff_t
#endif

namespace island::ipc {

/// @brief 传输接口
/// @note 同一实例服务端/客户端角色互斥：listen 后走 accept 路径，
///       connect 后走读写路径。
class ITransport {
   public:
    virtual ~ITransport() = default;

    /// @brief 服务端：创建监听端点（同一实例可重复调用以支持重连）
    virtual bool listen(const std::string& endpoint) = 0;

    /// @brief 服务端：阻塞等待客户端接入；成功返回 true
    /// @note stop() 期间返回 false（内部句柄被 close）
    virtual bool accept() = 0;

    /// @brief 客户端：连接服务端（短暂超时，失败返回 false）
    virtual bool connect(const std::string& endpoint) = 0;

    /// @brief 阻塞读；返回 0 = 对端关闭/EOF，<0 = 错误
    virtual ssize_t read(std::span<std::byte> buf) = 0;

    /// @brief 全量写；<0 = 错误
    virtual ssize_t write(std::span<const std::byte> data) = 0;

    /// @brief 关闭并释放内部句柄（幂等）
    virtual void close() = 0;

    /// @brief 是否有可用连接（客户端连接成功 / 服务端 accept 成功）
    [[nodiscard]] virtual bool is_connected() const = 0;
};

/// @brief 生成平台相关默认端点路径
/// @param pid TUI 进程 id
std::string default_endpoint(uint32_t pid);

/// @brief 创建服务端监听实例（平台分支工厂）
std::unique_ptr<ITransport> create_listener();

/// @brief 创建客户端连接实例（平台分支工厂）
std::unique_ptr<ITransport> create_connector();

}  // namespace island::ipc