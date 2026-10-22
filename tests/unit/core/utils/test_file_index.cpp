/**
 * @file test_file_index.cpp
 * @brief FileIndex 文件索引与异步构建单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "core/utils/file_index.h"

using namespace agent;

namespace {

/// 创建唯一临时目录（测试结束前由调用方清理）
std::filesystem::path make_temp_dir() {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / ("workx_file_index_" +
                       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

void touch(const std::filesystem::path& p) { std::ofstream(p).close(); }

}  // namespace

TEST_CASE("FileIndex sync build indexes files and skips dirs", "[core][file_index]") {
    auto root = make_temp_dir();
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / ".git");  // 应跳过
    touch(root / "main.cpp");
    touch(root / "src" / "util.h");

    FileIndex index;
    REQUIRE_FALSE(index.is_ready());
    index.build(root.string());
    REQUIRE(index.is_ready());
    REQUIRE(index.size() >= 2);

    auto results = index.search("main");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].name == "main.cpp");

    // .git 被跳过，不进入索引
    REQUIRE(index.search(".git").empty());

    std::filesystem::remove_all(root);
}

TEST_CASE("FileIndex async build via build_async + wait_ready", "[core][file_index]") {
    auto root = make_temp_dir();
    touch(root / "async_target.txt");

    FileIndex index;
    REQUIRE_FALSE(index.is_ready());
    index.build_async(root.string());
    REQUIRE(index.wait_ready(2000));
    REQUIRE(index.is_ready());

    auto results = index.search("async_target");
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].name == "async_target.txt");

    std::filesystem::remove_all(root);
}

TEST_CASE("FileIndex repeated build_async calls are safe", "[core][file_index]") {
    auto root = make_temp_dir();
    touch(root / "a.txt");

    FileIndex index;
    index.build_async(root.string());
    REQUIRE(index.wait_ready(2000));
    // 再次请求：允许重建（重建防抖由 refresh_if_needed 负责），
    // 连续调用应安全（不崩溃、不破坏索引），且最终就绪
    index.build_async(root.string());
    index.build_async(root.string());
    REQUIRE(index.wait_ready(2000));
    REQUIRE(index.is_ready());
    REQUIRE(index.search("a.txt").size() == 1);

    std::filesystem::remove_all(root);
}

TEST_CASE("FileIndex refresh_if_needed rebuilds async after dirty", "[core][file_index]") {
    auto root = make_temp_dir();
    touch(root / "first.txt");

    FileIndex index;
    index.build(root.string());
    REQUIRE(index.is_ready());
    REQUIRE(index.search("first.txt").size() == 1);
    REQUIRE_FALSE(index.search("second.txt").size());

    // 新增文件并标记脏 → refresh 触发后台重建（不阻塞 UI）
    touch(root / "second.txt");
    index.mark_dirty();
    REQUIRE(index.refresh_if_needed(0));

    // 后台重建为异步：轮询等待新文件进入索引（最多 2s）
    bool found = false;
    for (int i = 0; i < 200; ++i) {
        if (!index.search("second.txt").empty()) {
            found = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(found);

    std::filesystem::remove_all(root);
}

TEST_CASE("FileIndex wait_ready times out when never built", "[core][file_index]") {
    FileIndex index;
    REQUIRE_FALSE(index.wait_ready(50));
}

TEST_CASE("FileIndex shutdown is safe without running thread", "[core][file_index]") {
    FileIndex index;
    index.shutdown();  // 未启动线程，joinable()==false，不崩溃
    index.shutdown();  // 幂等
}
