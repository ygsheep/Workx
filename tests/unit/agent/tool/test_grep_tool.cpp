/**
 * @file test_grep_tool.cpp
 * @brief GrepTool 单元测试
 * @details 直接调用 GrepTool::call() 验证工具本身（而非底层 rg 命令）：
 *          - 元信息（name/description/prompt/schema）
 *          - 参数校验（缺 pattern / 空 pattern / 不存在路径）
 *          - 正则搜索（在临时文件上）
 *          - 字面量搜索（--fixed-strings）
 *          - 无匹配返回
 *          - glob 过滤
 *          - 大小写不敏感
 *
 *          搜索对象使用测试临时目录中的文件，避免依赖项目布局。
 */

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/tool/GrepTool/grep_tool.h"
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
    auto dir = tmp / ("workx_grep_test_" +
                      std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir);
    return dir;
}

/// 写入测试文件
void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream ofs(path, std::ios::binary);
    ofs << content;
}

/// 构建最小可用 ToolContext
void fill_ctx(ToolContext& ctx, const std::string& cwd) {
    ctx.cwd = cwd;
    ctx.session_id = "test";
    ctx.config_manager_ptr = &ConfigManager::instance();
}

}  // namespace

// ============================================================
// 元信息
// ============================================================

TEST_CASE("GrepTool name is Grep", "[tool][grep][metadata]") {
    GrepTool tool;
    REQUIRE(tool.name() == "Grep");
}

TEST_CASE("GrepTool description non-empty", "[tool][grep][metadata]") {
    GrepTool tool;
    REQUIRE_FALSE(tool.description().empty());
}

TEST_CASE("GrepTool prompt non-empty", "[tool][grep][metadata]") {
    GrepTool tool;
    REQUIRE_FALSE(tool.prompt().empty());
}

TEST_CASE("GrepTool input_schema has pattern required", "[tool][grep][metadata]") {
    GrepTool tool;
    auto schema = tool.input_schema();
    REQUIRE(schema.contains("type"));
    REQUIRE(schema["type"] == "object");
    REQUIRE(schema.contains("properties"));
    REQUIRE(schema["properties"].contains("pattern"));
}

// ============================================================
// 参数校验
// ============================================================

TEST_CASE("GrepTool rejects missing pattern", "[tool][grep][validation]") {
    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, std::filesystem::current_path().string());

    auto r = tool.call(nlohmann::json::object(), ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
}

TEST_CASE("GrepTool rejects empty pattern", "[tool][grep][validation]") {
    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, std::filesystem::current_path().string());

    auto r = tool.call(nlohmann::json{{"pattern", ""}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
}

TEST_CASE("GrepTool rejects nonexistent path", "[tool][grep][validation]") {
    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, std::filesystem::current_path().string());

    auto r = tool.call(nlohmann::json{{"pattern", "test"}, {"path", "/nonexistent_dir_xyz"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ResourceNotFound);
}

// ============================================================
// 搜索功能（临时文件）
// ============================================================

TEST_CASE("GrepTool regex search finds matches", "[tool][grep][search]") {
    auto dir = make_temp_dir();
    auto file = dir / "sample.cpp";
    write_file(file, "int main() {\n    return 42;\n}\n");

    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(nlohmann::json{{"pattern", "return"}, {"path", file.string()}}, ctx);
    REQUIRE(r.is_ok());
    const auto text = r.value().text;
    REQUIRE(text.find("return") != std::string::npos);
    REQUIRE(text.find("2:") != std::string::npos);  // return 在第 2 行（1-based）

    std::filesystem::remove_all(dir);
}

TEST_CASE("GrepTool no match returns friendly message", "[tool][grep][search]") {
    auto dir = make_temp_dir();
    auto file = dir / "sample.txt";
    write_file(file, "hello world\n");

    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(
        nlohmann::json{{"pattern", "nonexistent_pattern_xyz"}, {"path", file.string()}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("No matches found") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("GrepTool literal search with regex metacharacters", "[tool][grep][search]") {
    auto dir = make_temp_dir();
    auto file = dir / "data.txt";
    write_file(file, "value: a+b*c (test)\nplain line\n");

    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    // regex=false：按字面量匹配 "a+b*c"，正则语义下 + 和 * 是元字符
    auto r = tool.call(
        nlohmann::json{{"pattern", "a+b*c"}, {"path", file.string()}, {"regex", false}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("a+b*c") != std::string::npos);

    std::filesystem::remove_all(dir);
}

TEST_CASE("GrepTool glob filter narrows search", "[tool][grep][search]") {
    auto dir = make_temp_dir();
    write_file(dir / "a.cpp", "needle\n");
    write_file(dir / "b.h", "needle\n");
    write_file(dir / "c.txt", "needle\n");

    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    auto r = tool.call(
        nlohmann::json{{"pattern", "needle"}, {"path", dir.string()}, {"glob", "*.cpp"}}, ctx);
    REQUIRE(r.is_ok());
    const auto text = r.value().text;
    REQUIRE(text.find("a.cpp") != std::string::npos);
    REQUIRE(text.find("b.h") == std::string::npos);    // 被 glob 排除
    REQUIRE(text.find("c.txt") == std::string::npos);  // 被 glob 排除

    std::filesystem::remove_all(dir);
}

TEST_CASE("GrepTool case insensitive search", "[tool][grep][search]") {
    auto dir = make_temp_dir();
    auto file = dir / "case.txt";
    write_file(file, "Hello World\nfoo bar\n");

    GrepTool tool;
    ToolContext ctx;
    fill_ctx(ctx, dir.string());

    // 默认大小写敏感：HELLO 不应匹配 hello
    auto sensitive = tool.call(nlohmann::json{{"pattern", "HELLO"}, {"path", file.string()}}, ctx);
    REQUIRE(sensitive.is_ok());
    REQUIRE(sensitive.value().text.find("No matches") != std::string::npos);

    // case_insensitive=true：HELLO 匹配 hello
    auto insensitive = tool.call(
        nlohmann::json{{"pattern", "HELLO"}, {"path", file.string()}, {"case_insensitive", true}},
        ctx);
    REQUIRE(insensitive.is_ok());
    REQUIRE(insensitive.value().text.find("Hello") != std::string::npos);

    std::filesystem::remove_all(dir);
}
