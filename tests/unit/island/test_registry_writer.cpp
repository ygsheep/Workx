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

#include "core/utils/utf8_path.h"  // #182：中文 cwd → UTF-8
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

// #182 回归：project_root 此前由 fs::current_path().string() 填入 —— 中文路径在
// Windows 上是 GBK（非法 UTF-8）。to_json(...).dump() 的默认 strict 策略会抛
// nlohmann type_error.316，而该调用正处在 IslandServer::start 的启动路径上
// （crash log: registry_writer.cpp:64）→ 从中文名目录启动即崩。
TEST_CASE("registry: project_root 含非 UTF-8 字节时写入不抛异常（#182）", "[island][registry]") {
    const auto path = unique_registry_path();
    std::filesystem::remove(path);
    RegistryWriter writer(path);

    RegistryEntry e;
    e.pid = 7777;
    e.endpoint = "\\\\.\\pipe\\workx-island-7777";
    // "表情包" 的 GBK 字节，前缀保持 ASCII 便于定位
    e.project_root = std::string("D:/develop/") + "\xB1\xED\xC7\xE9\xB0\xFC";
    e.started_at = 1700000000;
    e.model = "deepseek-flash";
    e.last_heartbeat = 1700000001;

    REQUIRE(writer.write(e).is_ok());
    REQUIRE(std::filesystem::exists(path));

    // 落盘内容仍是合法 JSON（parse 成功），非法字节被替换为 U+FFFD
    const auto all = RegistryWriter::read_all(path);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().size() == 1);
    REQUIRE(all.value().front().project_root.find("\xEF\xBF\xBD") != std::string::npos);
}

#ifdef _WIN32
// #182 正解：中文 cwd 必须经 core::util::path_to_utf8() 再写注册表 ——
// 既保证 JSON 合法，又能**无损**往返（不是 U+FFFD 兜底）。
TEST_CASE("registry: 中文 project_root 经 path_to_utf8 后无损往返（#182）",
          "[island][registry][utf8]") {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / std::filesystem::path(L"workx_注册表_测试");
    std::filesystem::create_directories(dir);
    const auto path = dir / "registry_utf8.json";
    std::filesystem::remove(path);

    RegistryWriter writer(path);
    RegistryEntry e;
    e.pid = 8888;
    e.endpoint = "\\\\.\\pipe\\workx-island-8888";
    e.project_root = core::util::path_to_utf8(dir);
    e.started_at = 1700000000;
    e.model = "deepseek-flash";
    e.last_heartbeat = 1700000001;

    REQUIRE(writer.write(e).is_ok());

    const auto all = RegistryWriter::read_all(path);
    REQUIRE(all.is_ok());
    REQUIRE(all.value().front().project_root == core::util::path_to_utf8(dir));
    REQUIRE(all.value().front().project_root.find("\xEF\xBF\xBD") == std::string::npos);
}
#endif

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