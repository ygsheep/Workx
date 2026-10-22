/**
 * @file itransport.h
 * @brief Island IPC 传输层抽象（平台无关）
 * @details 阻塞 I/O + 线程模型（用户确认，替代设计文档的 asio）：
 *          - Windows:  named pipe  \\.\pipe\workx-island-<pid>
 *          - POSIX:    unix socket $XDG_RUNTIME_DIR (或 $TMPDIR / /tmp)
 *          服务端流程：listen → accept_connection(阻塞) → 服务该连接 → 回到
 *          accept_connection。**监听端点在整个生命周期内保持开放**（#118）：
 *          服务当前连接期间到来的新客户端进入 backlog 排队，而不是撞进
 *          「上一条连接已断开、监听端点还没重建」这个无监听者窗口。
 *          stop() 时 close() 使阻塞的 accept/read 返回失败，线程据此退出。
 * @version 1.1.0
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
/// @note 服务端实例与连接实例角色分离：listen() 后本实例只负责**监听**，
///       每次 accept_connection() 返回一个**独立的连接实例**（走读写路径）。
///       客户端实例则 listen/accept 均不参与，connect() 后直接读写。
class ITransport {
   public:
    virtual ~ITransport() = default;

    /// @brief 服务端：创建监听端点（同一实例可重复调用以支持重建）
    virtual bool listen(const std::string& endpoint) = 0;

    /// @brief 服务端：阻塞等待客户端接入，返回**独立**的连接实例
    /// @details 返回后监听端点**仍然开放**（实现内部按需重建监听实例），
    ///          因此服务当前连接期间新客户端会排队等待而不是被拒绝 —— 这是
    ///          #118 的根治点：端点全程有监听者，不存在无监听者窗口。
    /// @return 连接实例（仅用于 read/write/close）；stop() 期间或出错返回 nullptr
    /// @note 返回的实例不参与监听，析构时不会拆除监听端点
    virtual std::unique_ptr<ITransport> accept_connection() = 0;

    /// @brief 客户端：连接服务端（短暂超时，失败返回 false）
    virtual bool connect(const std::string& endpoint) = 0;

    /// @brief 阻塞读；返回 0 = 对端关闭/EOF，<0 = 错误
    virtual ssize_t read(std::span<std::byte> buf) = 0;

    /// @brief 全量写；<0 = 错误
    virtual ssize_t write(std::span<const std::byte> data) = 0;

    /// @brief 关闭并释放内部句柄（幂等）
    virtual void close() = 0;

    /// @brief 是否有可用连接（客户端 connect 成功；监听实例恒为 false）
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