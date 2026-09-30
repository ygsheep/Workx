/**
 * @file island_server.h
 * @brief Island IPC 服务端（TUI 侧）
 * @details 职责：
 *          - 监听 IPC 端点（named pipe / unix socket），服务单客户端（GUI）
 *          - 事件推送：publish_event() 分配单调 seq，入环形缓冲（上限
 *            ring_capacity 条，满则丢最旧），publisher 线程实时写出
 *          - 断线重连回放：hello 请求携带 last_seq，服务端回放缓冲中
 *            seq > last_seq 的事件后切到实时推送（seq 跳变由 GUI 负责补全）
 *          - 请求分发：hello / ping / subscribe 内置；其余请求转交
 *            RequestHandler 回调（main 接线 refresh_balance 等）
 *          线程模型：accept 线程（含连接读取循环）+ publisher 线程；
 *          stop() 关闭句柄打断阻塞 I/O，join 线程后清理 registry。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "island/ipc/itransport.h"
#include "island/registry_writer.h"
#include "island/jsonl_protocol.h"

namespace island {

/// @brief 服务端运行配置
struct IslandServerConfig {
    std::string endpoint;         ///< IPC 端点路径（默认 ipc::default_endpoint(pid)）
    uint32_t pid = 0;             ///< TUI 进程 id
    std::string project_root;     ///< TUI 工作目录（GUI 会话标识）
    std::string model;            ///< 当前模型名
    int64_t started_at = 0;       ///< 启动时间（Unix 秒）
    size_t ring_capacity = 1024;  ///< 回放环形缓冲容量（约 256KB）
};

/// @brief Island IPC 服务端
class IslandServer {
   public:
    /// @param registry 注册文件写入器（可选；注入后 ping 刷新心跳、stop 移除记录）
    IslandServer(IslandServerConfig cfg, std::unique_ptr<ipc::ITransport> listener = nullptr,
                 RegistryWriter* registry = nullptr);

    IslandServer(const IslandServer&) = delete;
    IslandServer& operator=(const IslandServer&) = delete;
    ~IslandServer();

    /// @brief 启动 accept + publisher 线程并写入注册文件
    void start();

    /// @brief 停止服务：断开连接、join 线程、移除 registry 记录
    void stop();

    /// @brief 是否运行中
    [[nodiscard]] bool is_running() const { return m_running.load(); }

    /// @brief 推送事件（分配 seq 入缓冲，广播给当前连接）
    /// @note 线程安全：bridge 在主循环线程调用，publisher 独立线程消费
    void publish_event(const std::string& type, const nlohmann::json& data);

    /// @brief 请求处理器（main 接线 refresh_balance / get_model_pricing /
    ///        get_session_summary 等）
    /// @return 响应 data；返回 json null 表示未支持的请求（响应 ok=false）
    using RequestHandler =
        std::function<nlohmann::json(const std::string& type, const nlohmann::json& data)>;
    void set_request_handler(RequestHandler handler) { m_request_handler = std::move(handler); }

    /// @brief 当前缓冲条目数（诊断/测试）
    [[nodiscard]] size_t ring_size() const;

   private:
    void accept_loop();
    void publisher_loop();
    void handle_connection(const std::shared_ptr<ipc::ITransport>& conn);
    void handle_request(const std::shared_ptr<ipc::ITransport>& conn, const Envelope& env);
    void replay_from(const std::shared_ptr<ipc::ITransport>& conn, int64_t last_seq);
    void write_lines(const std::shared_ptr<ipc::ITransport>& conn,
                     const std::vector<std::string>& lines);
    void refresh_registry_heartbeat();

    IslandServerConfig m_cfg;
    std::unique_ptr<ipc::ITransport> m_listener;
    RegistryWriter* m_registry = nullptr;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};

    /// @brief 回放缓冲（seq + 序列化行），mutex 保护
    struct RingEntry {
        int64_t seq;
        std::string line;
    };
    std::deque<RingEntry> m_ring;
    mutable std::mutex m_ring_mutex;
    std::atomic<int64_t> m_seq{0};

    /// @brief 当前连接（accept 线程写入，publisher 读取）
    std::shared_ptr<ipc::ITransport> m_conn;
    std::mutex m_conn_mutex;
    /// @brief 连接写互斥（回放与实时推送串行）
    std::mutex m_write_mutex;
    /// @brief 实时推送游标（下次发布的起始 seq）
    std::atomic<int64_t> m_next_seq{1};
    /// @brief 当前连接是否完成 hello 握手（握手前不推送，防重复）
    std::atomic<bool> m_hello_received{false};

    std::condition_variable m_cv;
    std::mutex m_cv_mutex;

    std::atomic<bool> m_listening_ok{false};
    std::condition_variable m_listening_cv;
    std::mutex m_listening_mutex;

    std::thread m_accept_thread;
    std::thread m_publisher_thread;

    RequestHandler m_request_handler;
    int64_t m_last_heartbeat_write = 0;  ///< 心跳写 registry 节流（10s）
};

}  // namespace island