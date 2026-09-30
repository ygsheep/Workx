/**
 * @file test_registry_writer.cpp
 * @brief 注册文件读写单测
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "island/registry_writer.h"

using island::RegistryEntry;
using island::RegistryWriter;

namespace {

std::filesystem::path unique_registry_path() {
    const auto dir = std::filesystem::temp_directory_path() / "workx_island_test";
    std::filesystem::create_directories(dir);
    return dir / "registry_test.json";
}

}  // namespace

TEST_CASE("registry: write then read_all round trip (single entry)", "[island][registry]") {
    const auto path = unique_registry_path();
    std::filesystem::remove(path);
    RegistryWriter writer(path);

    RegistryEntry e;
    e.pid = 4242;
    e.endpoint = "\\\\.\\pipe\\workx-island-4242";
    e.project_root = "D:/proj";
    e.started_at = 1700000000;
    e.model = "deepseek-chat";
    e.last_heartbeat = 1700000001;

    REQUIRE(writer.write(e).is_ok());

    const auto all = RegistryWriter::read_all(path);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().size() == 1);
    const auto& got = all.value().front();
    REQUIRE(got.pid == 4242);
    REQUIRE(got.endpoint == e.endpoint);
    REQUIRE(got.project_root == "D:/proj");
    REQUIRE(got.started_at == 1700000000);
    REQUIRE(got.model == "deepseek-chat");
    REQUIRE(got.last_heartbeat == 1700000001);
}

TEST_CASE("registry: multi entries, remove pid, rewrite keeps others", "[island][registry]") {
    const auto path = unique_registry_path();
    std::filesystem::remove(path);
    RegistryWriter writer(path);

    RegistryEntry a;
    a.pid = 100;
    a.endpoint = "ep-a";
    RegistryEntry b;
    b.pid = 200;
    b.endpoint = "ep-b";
    REQUIRE(writer.write(a).is_ok());
    REQUIRE(writer.write(b).is_ok());

    auto all = RegistryWriter::read_all(path);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().size() == 2);

    REQUIRE(writer.remove(100).is_ok());
    all = RegistryWriter::read_all(path);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().size() == 1);
    REQUIRE(all.value().front().pid == 200);

    REQUIRE(writer.remove(999).is_ok());  // 移除不存在 pid 不报错
}

TEST_CASE("registry: read_all on missing file returns empty, not error", "[island][registry]") {
    const auto path = unique_registry_path();
    std::filesystem::remove(path);
    const auto all = RegistryWriter::read_all(path);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().empty());
}

TEST_CASE("registry: corrupted file yields error", "[island][registry]") {
    const auto path = unique_registry_path();
    std::filesystem::create_directories(path.parent_path());
    {
        std::ofstream ofs(path, std::ios::trunc);
        ofs << "{not valid json";
    }
    auto result = RegistryWriter::read_all(path);
    REQUIRE(result.is_err());
}

TEST_CASE("registry: to_json / parse round trip", "[island][registry]") {
    RegistryEntry e;
    e.pid = 7;
    e.endpoint = "ep";
    e.model = "m";
    const nlohmann::json j = RegistryWriter::to_json({e});
    REQUIRE(j["sessions"].size() == 1);

    const auto parsed = RegistryWriter::parse(j.dump());
    REQUIRE(parsed.is_ok());
    REQUIRE(parsed.value().size() == 1);
    REQUIRE(parsed.value().front().pid == 7);
    REQUIRE(parsed.value().front().endpoint == "ep");
}

TEST_CASE("registry: default path is under home/.workx", "[island][registry]") {
    const auto path = RegistryWriter::default_registry_path();
    REQUIRE(path.filename() == "island.registry");
    REQUIRE(path.parent_path().filename() == ".workx");
}