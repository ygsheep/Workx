/**
 * @file test_ipc_transport.cpp
 * @brief IPC 传输层集成单测（本机回环）
 * @note 使用真实 named pipe / unix socket 本机回环，不依赖外部服务。
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "island/ipc/itransport.h"

namespace {

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

/// @brief 测试端点：默认端点 + 随机后缀（避免多测试并发冲突）
std::string unique_endpoint() {
    const auto suffix =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    return island::ipc::default_endpoint(current_pid()) + "-t" + std::to_string(suffix % 1000000);
}

std::string as_string(std::span<std::byte> bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

TEST_CASE("ipc: listener/connector echo round trip", "[island][ipc]") {
    auto listener = island::ipc::create_listener();
    REQUIRE(listener != nullptr);
    const std::string ep = unique_endpoint();
    REQUIRE(listener->listen(ep));

    std::string server_msg;
    std::thread server_thread([&] {
        REQUIRE(listener->accept());
        std::vector<std::byte> buf(64);
        const auto n = listener->read(buf);
        REQUIRE(n > 0);
        server_msg = as_string(std::span(buf).first(static_cast<size_t>(n)));
        REQUIRE(listener->write(std::span(buf).first(static_cast<size_t>(n))) >
                0);  // echo 回客户端
    });

    auto connector = island::ipc::create_connector();
    REQUIRE(connector != nullptr);
    REQUIRE(connector->connect(ep));

    const std::string msg = "hello island";
    REQUIRE(connector->write(std::as_bytes(std::span(msg))) == static_cast<ssize_t>(msg.size()));

    std::vector<std::byte> buf(64);
    const auto n = connector->read(buf);
    REQUIRE(n == static_cast<ssize_t>(msg.size()));
    REQUIRE(as_string(std::span(buf).first(static_cast<size_t>(n))) == msg);

    connector->close();
    server_thread.join();
    REQUIRE(server_msg == msg);
}

TEST_CASE("ipc: close unblocks accept", "[island][ipc]") {
    auto listener = island::ipc::create_listener();
    REQUIRE(listener != nullptr);
    const std::string ep = unique_endpoint();
    REQUIRE(listener->listen(ep));

    std::atomic<bool> accept_returned{false};
    std::thread server_thread([&] {
        listener->accept();  // 阻塞等待客户端
        accept_returned.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    listener->close();
    server_thread.join();
    REQUIRE(accept_returned.load());
}

TEST_CASE("ipc: connect to nonexistent endpoint fails", "[island][ipc]") {
    auto connector = island::ipc::create_connector();
    REQUIRE(connector != nullptr);
    const std::string ghost = island::ipc::default_endpoint(current_pid()) + "-ghost";
    REQUIRE_FALSE(connector->connect(ghost));
}
