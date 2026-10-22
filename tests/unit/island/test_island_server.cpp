/**
 * @file test_island_server.cpp
 * @brief Island 服务端 + 客户端集成单测（JSONL over IPC）
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "island/ipc/itransport.h"
#include "island/island_server.h"
#include "island/jsonl_protocol.h"
#include "island/registry_writer.h"

using island::Envelope;
using island::IslandServer;
using island::IslandServerConfig;
using island::parse_line;
using island::serialize_request;

namespace {

/// @brief 测试客户端：连接 + 读写一行
class TestClient {
   public:
    explicit TestClient(const std::string& endpoint) : m_conn(island::ipc::create_connector()) {
        CHECK((m_conn && m_conn->connect(endpoint)));
    }

    /// @brief 发送请求，读取直至收到匹配 id 的响应
    [[nodiscard]] std::optional<Envelope> request(const std::string& type,
                                                  const nlohmann::json& data) {
        const std::string id = "r" + std::to_string(m_counter++);
        const std::string line = serialize_request(type, data, id);
        CHECK(m_conn->write(std::as_bytes(std::span(line))) > 0);
        return read_until(
            [&](const Envelope& e) { return e.kind == island::MsgKind::Response && e.id == id; });
    }

    /// @brief 阻塞读满一行（容忍粘包：缓冲剩余数据供下次读取）
    /// @note 读失败（n <= 0，对端关闭/出错）时置 m_dead，供 read_until 提前退出
    [[nodiscard]] std::optional<Envelope> read_line(
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            const size_t nl = m_buf.find('\n');
            if (nl != std::string::npos) {
                const std::string line = m_buf.substr(0, nl);
                m_buf.erase(0, nl + 1);
                return parse_line(line);
            }
            std::vector<std::byte> buf(65536);
            const auto n = m_conn->read(buf);
            if (n <= 0) {
                m_dead = true;
                return std::nullopt;
            }
            m_buf.append(reinterpret_cast<const char*>(buf.data()), static_cast<size_t>(n));
        }
        return std::nullopt;
    }

   private:
    template <typename F>
    std::optional<Envelope> read_until(F&& pred) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline) {
            auto env = read_line(std::chrono::milliseconds(300));
            if (env && pred(*env)) return env;
            // #118：连接已断（read 立即失败）时不再空转满 3 秒 —— 那会把
            // 「连接失败」包装成「看起来像超时」，掩盖真实原因、拖慢失败反馈。
            if (m_dead) break;
        }
        return std::nullopt;
    }

    std::unique_ptr<island::ipc::ITransport> m_conn;
    std::string m_buf;  ///< 粘包缓冲（跨 read 保留未解析字节）
    uint64_t m_counter = 0;
    bool m_dead = false;  ///< read 已失败（对端关闭/出错）

   public:
    [[nodiscard]] const std::string& debug_buf() const { return m_buf; }
};

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

uint32_t current_pid() {
#ifdef _WIN32
    return static_cast<uint32_t>(GetCurrentProcessId());
#else
    return static_cast<uint32_t>(getpid());
#endif
}

std::string unique_endpoint() {
    return island::ipc::default_endpoint(current_pid()) + "-srv" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 1000000);
}

std::filesystem::path unique_registry_path() {
    const auto dir = std::filesystem::temp_directory_path() / "workx_island_test";
    std::filesystem::create_directories(dir);
    return dir / ("server_registry_" + std::to_string(current_pid()) + ".json");
}

}  // namespace

TEST_CASE("server: hello handshake returns metadata", "[island][server]") {
    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    cfg.pid = 0;
    cfg.project_root = "D:/demo";
    cfg.model = "deepseek-chat";
    IslandServer server(std::move(cfg));
    server.start();

    TestClient client(ep);
    auto resp = client.request("hello", {{"last_seq", 0}});
    REQUIRE(resp.has_value());
    REQUIRE(resp->ok);
    REQUIRE(resp->data["pid"] == current_pid());
    REQUIRE(resp->data["project_root"] == "D:/demo");
    REQUIRE(resp->data["model"] == "deepseek-chat");
    REQUIRE(!resp->data["tui_version"].is_null());

    server.stop();
}

TEST_CASE("server: events are pushed with monotonic seq after hello", "[island][server]") {
    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    IslandServer server(std::move(cfg));
    server.start();

    TestClient client(ep);
    REQUIRE(client.request("hello", {{"last_seq", 0}}).has_value());

    server.publish_event("tool_call", {{"call_id", "c1"}, {"tool_name", "Bash"}});
    server.publish_event("message_delta", {{"delta_text", "hi"}});

    auto e1 = client.read_line();
    REQUIRE(e1.has_value());
    REQUIRE(e1->kind == island::MsgKind::Event);
    REQUIRE(e1->type == "tool_call");
    REQUIRE(e1->seq == 1);
    REQUIRE(e1->data["call_id"] == "c1");

    auto e2 = client.read_line();
    if (!e2.has_value()) {
        fprintf(stderr, "[tc] e2 missing, buffered=[%s]\n", client.debug_buf().c_str());
    }
    REQUIRE(e2.has_value());
    REQUIRE(e2->type == "message_delta");
    REQUIRE(e2->seq == 2);

    server.stop();
}

TEST_CASE("server: ring buffer replays missed events on reconnect", "[island][server]") {
    auto registry_path = unique_registry_path();
    std::filesystem::remove(registry_path);
    island::RegistryWriter registry_writer(registry_path);

    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    cfg.pid = 0;
    cfg.ring_capacity = 64;
    IslandServer server(std::move(cfg), nullptr, &registry_writer);
    server.start();

    // 第一条连接：hello 前的事件不推送，直接入环
    auto client = std::make_unique<TestClient>(ep);
    REQUIRE(client->request("hello", {{"last_seq", 0}}).has_value());

    server.publish_event("a", {{"n", 1}});
    auto a = client->read_line();
    REQUIRE((a.has_value() && a->seq == 1));

    // 断开：close 客户端，服务端 accept 循环等待重连
    client.reset();
    auto client2 = std::make_unique<TestClient>(ep);
    REQUIRE(client2->request("hello", {{"last_seq", 1}}).has_value());

    server.publish_event("b", {{"n", 2}});
    auto b = client2->read_line();
    REQUIRE((b.has_value() && b->seq == 2));

    // 断开重连：last_seq=0 → 回放环内全部（1 和 2）
    client2.reset();
    auto client3 = std::make_unique<TestClient>(ep);
    REQUIRE(client3->request("hello", {{"last_seq", 0}}).has_value());
    auto r1 = client3->read_line();
    REQUIRE((r1.has_value() && r1->seq == 1));
    auto r2 = client3->read_line();
    REQUIRE((r2.has_value() && r2->seq == 2));

    server.stop();
}

TEST_CASE("server: ping refreshes heartbeat and responds", "[island][server]") {
    auto registry_path = unique_registry_path();
    std::filesystem::remove(registry_path);
    island::RegistryWriter registry_writer(registry_path);

    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    IslandServer server(std::move(cfg), nullptr, &registry_writer);
    server.start();

    TestClient client(ep);
    REQUIRE(client.request("hello", {{"last_seq", 0}}).has_value());
    auto pong = client.request("ping", {});
    REQUIRE(pong.has_value());
    REQUIRE(pong->ok);
    REQUIRE(pong->data["pong"] == true);

    server.stop();
}

TEST_CASE("server: custom request handler and unsupported fallback", "[island][server]") {
    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    IslandServer server(std::move(cfg));
    server.set_request_handler([](const std::string& type, const nlohmann::json& data) {
        if (type == "refresh_balance") {
            return nlohmann::json{{"balance_usd", 12.34}, {"source", "test"}};
        }
        return nlohmann::json(nullptr);  // 未支持
    });
    server.start();

    TestClient client(ep);
    REQUIRE(client.request("hello", {{"last_seq", 0}}).has_value());

    auto bal = client.request("refresh_balance", {});
    REQUIRE(bal.has_value());
    REQUIRE(bal->ok);
    REQUIRE(bal->data["balance_usd"] == 12.34);

    auto unk = client.request("bogus_request", {});
    REQUIRE(unk.has_value());
    REQUIRE_FALSE(unk->ok);
    REQUIRE(unk->data["error"] == "unsupported request: bogus_request");

    server.stop();
}

TEST_CASE("server: rapid consecutive reconnects never hit a no-listener window",
          "[island][server][issue118]") {
    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    cfg.pid = 0;
    cfg.ring_capacity = 64;
    IslandServer server(std::move(cfg));
    server.start();

    // #118 回归点：连续重连 6 次，每次都必须立刻完成 hello 握手。
    // 旧实现在「上一条连接断开 → 重新 listen」之间存在**无监听者窗口**，重连
    // 客户端撞进去只能靠 connect() 的固定重试硬扛（POSIX 侧总窗口 600ms），
    // 共享 runner 调度抖动一超即失败 —— CI 上实测同一条重连用例 8 次里红 3 次。
    // 现在监听端点全程开放，新连接进 backlog 排队，不存在该窗口。
    for (int i = 0; i < 6; ++i) {
        auto client = std::make_unique<TestClient>(ep);
        REQUIRE(client->request("hello", {{"last_seq", 0}}).has_value());
    }  // 每次迭代末尾 client 析构 → 断开，下一轮重连

    server.stop();
}

TEST_CASE("server: is_running lifecycle", "[island][server]") {
    const auto ep = unique_endpoint();
    IslandServerConfig cfg;
    cfg.endpoint = ep;
    IslandServer server(std::move(cfg));
    REQUIRE_FALSE(server.is_running());
    server.start();
    REQUIRE(server.is_running());
    server.stop();
    REQUIRE_FALSE(server.is_running());
}