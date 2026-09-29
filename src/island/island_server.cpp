/**
 * @file island_server.cpp
 * @brief Island IPC 服务端实现
 * @version 1.0.1
 * @date 2026-08
 */

#include "island/island_server.h"

#include <algorithm>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "island/jsonl_protocol.h"

namespace island {

namespace {

constexpr int64_t kHeartbeatWriteThrottleSec = 10;

uint32_t current_pid() noexcept {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

}  // namespace

IslandServer::IslandServer(IslandServerConfig cfg, std::unique_ptr<ipc::ITransport> listener,
                           RegistryWriter* registry)
    : m_cfg(std::move(cfg)),
      m_listener(listener ? std::move(listener) : ipc::create_listener()),
      m_registry(registry) {
    if (m_cfg.pid == 0) {
        m_cfg.pid = current_pid();
    }
    if (m_cfg.endpoint.empty()) {
        m_cfg.endpoint = ipc::default_endpoint(m_cfg.pid);
    }
    if (m_cfg.started_at == 0) {
        m_cfg.started_at = static_cast<int64_t>(now_ts());
    }
}

IslandServer::~IslandServer() { stop(); }

void IslandServer::start() {
    if (m_running.exchange(true)) return;
    m_stop.store(false);
    m_publisher_thread = std::thread([this] { publisher_loop(); });
    m_accept_thread = std::thread([this] { accept_loop(); });
    // 等待监听端点就绪（accept 线程完成 CreateNamedPipe），避免客户端抢连失败
    {
        std::unique_lock<std::mutex> lock(m_listening_mutex);
        m_listening_cv.wait_for(lock, std::chrono::seconds(5),
                                [this] { return m_listening_ok.load() || m_stop.load(); });
    }
    // 启动即写入注册文件（GUI 扫描发现本 TUI）
    if (m_registry) {
        RegistryEntry entry{
            .pid = m_cfg.pid,
            .endpoint = m_cfg.endpoint,
            .project_root = m_cfg.project_root,
            .started_at = m_cfg.started_at,
            .model = m_cfg.model,
            .last_heartbeat = static_cast<int64_t>(now_ts()),
        };
        m_registry->write(entry);
    }
}

void IslandServer::stop() {
    if (!m_running.exchange(false)) return;
    m_stop.store(true);
    m_cv.notify_all();

    // 关闭监听/连接句柄，打断阻塞的 accept/read
    {
        std::lock_guard<std::mutex> lock(m_conn_mutex);
        if (m_listener) m_listener->close();
        if (m_conn) m_conn->close();
    }
    if (m_accept_thread.joinable()) m_accept_thread.join();
    if (m_publisher_thread.joinable()) m_publisher_thread.join();

    if (m_registry) {
        m_registry->remove(m_cfg.pid);
    }
}

void IslandServer::publish_event(const std::string& type, const nlohmann::json& data) {
    if (!m_running.load()) return;
    const int64_t seq = ++m_seq;
    const std::string line = serialize_event(type, data, seq, now_ts());

    std::lock_guard<std::mutex> lock(m_ring_mutex);
    m_ring.push_back(RingEntry{seq, line});
    while (m_ring.size() > m_cfg.ring_capacity) {
        m_ring.pop_front();
    }
    m_cv.notify_all();
}

size_t IslandServer::ring_size() const {
    std::lock_guard<std::mutex> lock(m_ring_mutex);
    return m_ring.size();
}

// ============================================================
// 线程主循环
// ============================================================

void IslandServer::accept_loop() {
    // 循环：listen → accept → 服务连接 → 重新 listen（支持 GUI 重连）
    while (!m_stop.load()) {
        if (!m_listener->listen(m_cfg.endpoint)) {
            if (m_stop.load()) break;
            // 端点创建失败（如残留文件绑定冲突）：短暂退避后重试
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(m_listening_mutex);
            m_listening_ok.store(true);
        }
        m_listening_cv.notify_all();
        // stop 已触发则不再 accept（listen 窗口创建的新句柄成孤儿时随进程回收）
        if (m_stop.load()) break;

        if (!m_listener->accept()) {
            if (m_stop.load()) break;
            // accept 被关闭句柄打断（stop）或异常：直接退出循环
            break;
        }

        // accept 成功后 listener 已转化为连接，创建独立实例供下轮监听
        // （注：传输实现内 accept 后 close 了监听端点）
        std::shared_ptr<ipc::ITransport> conn;
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            conn = std::shared_ptr<ipc::ITransport>(std::move(m_listener));
            m_listener = ipc::create_listener();
            m_conn = conn;
            // 连接建立后先不推送，等 hello 握手指定 last_seq（防重复）
            m_hello_received.store(false);
        }
        handle_connection(conn);
        {
            std::lock_guard<std::mutex> lock(m_conn_mutex);
            m_conn.reset();
        }
    }
}

void IslandServer::publisher_loop() {
    while (!m_stop.load()) {
        // 等待：有新事件（ring 非空）且游标落后于最新 seq 且有连接
        std::unique_lock<std::mutex> lock(m_cv_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(200), [this] {
            if (m_stop.load()) return true;
            std::lock_guard<std::mutex> ring_lock(m_ring_mutex);
            if (m_ring.empty()) return false;
            std::lock_guard<std::mutex> conn_lock(m_conn_mutex);
            return static_cast<bool>(m_conn) && m_hello_received.load() &&
                   m_ring.back().seq >= m_next_seq.load();
        });

        if (m_stop.load()) break;

        std::vector<std::string> lines;
        {
            std::lock_guard<std::mutex> ring_lock(m_ring_mutex);
            if (m_ring.empty()) continue;
            const int64_t sentinel = m_ring.back().seq;
            for (const auto& e : m_ring) {
                if (e.seq >= m_next_seq.load()) lines.push_back(e.line);
            }
            m_next_seq.store(sentinel + 1);
        }
        std::shared_ptr<ipc::ITransport> conn;
        {
            std::lock_guard<std::mutex> conn_lock(m_conn_mutex);
            conn = m_conn;
        }
        if (conn && !lines.empty()) {
            write_lines(conn, lines);
        }
    }
}

void IslandServer::handle_connection(const std::shared_ptr<ipc::ITransport>& conn) {
    std::string buffer;  // 行缓冲（read 不保证按行到达）
    std::vector<std::byte> chunk(4096);
    while (!m_stop.load()) {
        const ssize_t n = conn->read(chunk);
        if (n <= 0) break;  // EOF / 错误
        buffer.append(reinterpret_cast<const char*>(chunk.data()), static_cast<size_t>(n));

        size_t pos = 0;
        while (true) {
            const size_t nl = buffer.find('\n', pos);
            if (nl == std::string::npos) break;
            std::string line = buffer.substr(pos, nl - pos);
            pos = nl + 1;
            if (line.empty() || line.back() == '\r') {
                if (!line.empty()) line.pop_back();
            }
            if (line.empty()) continue;

            auto env = parse_line(line);
            if (!env) continue;  // JSON 解析失败：丢弃该行
            if (env->kind == MsgKind::Request) {
                handle_request(conn, *env);
            }
            // Response/Event：GUI 不应发送，忽略
        }
        buffer.erase(0, pos);
    }
}

void IslandServer::handle_request(const std::shared_ptr<ipc::ITransport>& conn,
                                  const Envelope& env) {
    const std::string type = env.type;
    const std::string id = env.id;
    const nlohmann::json data = env.data;

    // 信封受损（缺 id）：无法关联响应，直接忽略
    if (id.empty()) return;

    if (type == "hello") {
        const int64_t last_seq = data.value("last_seq", static_cast<int64_t>(0));
        m_hello_received.store(true);
        const nlohmann::json resp{
#ifdef WORKX_BUILD_INFO
            {"tui_version", "workx-" WORKX_BUILD_INFO
#ifdef WORKX_FILE_VERSION
                            " (files: " WORKX_FILE_VERSION ")"
#endif
            },
#elif defined(WORKX_VERSION)
            {"tui_version", "workx-" WORKX_VERSION},
#else
            {"tui_version", "workx"},
#endif
            {"pid", m_cfg.pid},
            {"project_root", m_cfg.project_root},
            {"model", m_cfg.model},
            {"started_at", m_cfg.started_at},
            {"ring_size", static_cast<int64_t>(ring_size())},
        };
        // 先响应后回放：GUI 先完成握手，再按 seq 消费事件流
        write_lines(conn, {serialize_response(id, true, resp)});
        replay_from(conn, last_seq);
        return;
    }

    if (type == "ping") {
        refresh_registry_heartbeat();
        const nlohmann::json resp{{"pong", true}, {"tui_pid", m_cfg.pid}};
        write_lines(conn, {serialize_response(id, true, resp)});
        return;
    }

    if (type == "subscribe") {
        // 全量推送：订阅过滤由 GUI 本地按 type 过滤，服务端无需状态
        write_lines(conn, {serialize_response(id, true, nlohmann::json{{"ok", true}})});
        return;
    }

    // 其余请求（refresh_balance / get_model_pricing / get_session_summary）→ 回调
    if (!m_request_handler) {
        write_lines(
            conn, {serialize_response(id, false,
                                      nlohmann::json{{"error", "unsupported request: " + type}})});
        return;
    }
    nlohmann::json result = m_request_handler(type, data);
    if (result.is_null()) {
        write_lines(
            conn, {serialize_response(id, false,
                                      nlohmann::json{{"error", "unsupported request: " + type}})});
        return;
    }
    write_lines(conn, {serialize_response(id, true, result)});
}

void IslandServer::replay_from(const std::shared_ptr<ipc::ITransport>& conn, int64_t last_seq) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(m_ring_mutex);
        for (const auto& e : m_ring) {
            if (e.seq > last_seq) lines.push_back(e.line);
        }
        // 修正实时推送游标：回放完成后从缓冲最新位置继续
        if (!lines.empty()) {
            const int64_t sentinel = m_ring.back().seq;
            m_next_seq.store(std::max<int64_t>(m_next_seq.load(), sentinel + 1));
        }
    }
    write_lines(conn, lines);
}

void IslandServer::write_lines(const std::shared_ptr<ipc::ITransport>& conn,
                               const std::vector<std::string>& lines) {
    std::lock_guard<std::mutex> lock(m_write_mutex);
    for (const auto& line : lines) {
        if (line.size() > static_cast<size_t>(65536)) continue;  // 防呆：跳过超大行
        const ssize_t n = conn->write(std::as_bytes(std::span(line)));
        if (n < 0) return;
    }
}

void IslandServer::refresh_registry_heartbeat() {
    if (!m_registry) return;
    const int64_t now = static_cast<int64_t>(now_ts());
    if (now - m_last_heartbeat_write < kHeartbeatWriteThrottleSec) return;
    m_last_heartbeat_write = now;
    RegistryEntry entry{
        .pid = m_cfg.pid,
        .endpoint = m_cfg.endpoint,
        .project_root = m_cfg.project_root,
        .started_at = m_cfg.started_at,
        .model = m_cfg.model,
        .last_heartbeat = now,
    };
    m_registry->write(entry);
}

}  // namespace island