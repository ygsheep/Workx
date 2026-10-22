/**
 * @file test_glob_tool.cpp
 * @brief GlobTool 单元测试
 * @details 直接调用 GlobTool::call() 验证工具本身：
 *          - 元信息（name/description/schema）
 *          - 参数校验（缺 pattern / 空 pattern / 不存在目录）
 *          - 基础匹配
 *          - #189 扫描护栏：跳过依赖/构建/VCS 目录、结果上限截断
 * @version 1.0.0
 * @date 2026-10
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/tool/GlobTool/glob_tool.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/config/config_manager.h"
#include "core/utils/error.h"

using namespace agent;
using namespace agent::tool;

namespace {

/// 创建测试临时目录
std::filesystem::path make_temp_dir() {
    auto tmp = std::filesystem::temp_directory_path();
    auto dir = tmp / ("workx_glob_test_" +
                      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

/// 写入测试文件（自动建父目录）
void write_file(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path, std::ios::binary);
    ofs << "x";
}

/// 构建最小可用 ToolContext
void fill_ctx(ToolContext& ctx, const std::string& cwd) {
    ctx.cwd = cwd;
    ctx.session_id = "test";
    ctx.config_manager_ptr = &ConfigManager::instance();
}

}  // namespace

// ============================================================
// 元信息与校验
// ============================================================

TEST_CASE("GlobTool name is Glob", "[tool][glob][metadata]") {
    GlobTool tool;
    REQUIRE(tool.name() == "Glob");
    REQUIRE_FALSE(tool.description().empty());
}

// 空 pattern 的守卫在 validate_input（由 ToolExecutor 在 call 之前调用），
// 不在 call() 内部 —— 所以这里直接测 validate_input。
TEST_CASE("GlobTool validate_input rejects empty pattern", "[tool][glob][validation]") {
    GlobTool tool;
    ToolContext ctx;
    fill_ctx(ctx, std::filesystem::current_path().string());

    auto bad = tool.validate_input(nlohmann::json{{"pattern", ""}}, ctx);
    REQUIRE(bad.is_err());
    REQUIRE(bad.error().code == Error::Code::InvalidInput);

    auto ok = tool.validate_input(nlohmann::json{{"pattern", "*.txt"}}, ctx);
    REQUIRE(ok.is_ok());
}

TEST_CASE("GlobTool rejects nonexistent directory", "[tool][glob][validation]") {
    GlobTool tool;
    ToolContext ctx;
    fill_ctx(ctx, std::filesystem::current_path().string());

    auto r =
        tool.call(nlohmann::json{{"pattern", "*"}, {"cwd", "/nonexistent_dir_workx_xyz"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ResourceNotFound);
}

// ============================================================
// 匹配与扫描护栏（#189）
// ============================================================

TEST_CASE("GlobTool matches nested files", "[tool][glob][scan]") {
    const auto dir = make_temp_dir();
    write_file(dir / "src" / "keep.txt");

    GlobTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(nlohmann::json{{"pattern", "**/*.txt"}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("src/keep.txt") != std::string::npos);
}

TEST_CASE("GlobTool skips dependency/build/VCS directories", "[tool][glob][scan]") {
    const auto dir = make_temp_dir();
    write_file(dir / "src" / "keep.txt");
    write_file(dir / ".git" / "hidden.txt");
    write_file(dir / "node_modules" / "pkg" / "dep.txt");
    write_file(dir / "build" / "out.txt");

    GlobTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(nlohmann::json{{"pattern", "**/*.txt"}}, ctx);
    REQUIRE(r.is_ok());
    const std::string& out = r.value().text;
    CHECK(out.find("src/keep.txt") != std::string::npos);
    CHECK(out.find("hidden.txt") == std::string::npos);
    CHECK(out.find("dep.txt") == std::string::npos);
    CHECK(out.find("out.txt") == std::string::npos);
}

TEST_CASE("GlobTool truncates results beyond the cap with a notice", "[tool][glob][scan]") {
    const auto dir = make_temp_dir();
    constexpr int kFiles = 250;  // > kMaxMatches(200)
    for (int i = 0; i < kFiles; ++i) {
        write_file(dir / ("f" + std::to_string(i) + ".txt"));
    }

    GlobTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(nlohmann::json{{"pattern", "*.txt"}}, ctx);
    REQUIRE(r.is_ok());
    const std::string& out = r.value().text;

    // 截断到 200 行，并在末尾注明（保留"最新优先"语义）
    size_t lines = 0;
    size_t pos = 0;
    while ((pos = out.find(".txt", pos)) != std::string::npos) {
        ++lines;
        pos += 4;
    }
    CHECK(lines == 200u);
    CHECK(out.find("truncated: showing newest 200 of 250 matches") != std::string::npos);
}

TEST_CASE("GlobTool reports no match", "[tool][glob][scan]") {
    const auto dir = make_temp_dir();

    GlobTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(nlohmann::json{{"pattern", "*.txt"}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("No files matched pattern") != std::string::npos);
}
