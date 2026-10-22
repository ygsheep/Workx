/**
 * @file test_tool_registry.cpp
 * @brief agent::process::ToolRegistry 单元测试
 * @details 覆盖 resolve_ripgrep / resolve_tool / clear_cache / 缓存行为。
 *          由于 ToolRegistry 是单例且依赖运行时环境（exe 目录、PATH），
 *          测试以"行为契约"为主，不断言具体路径。
 */

#include <catch2/catch_test_macros.hpp>

#include "core/process/tool_registry.h"

#include <filesystem>
#include <optional>
#include <string>

using namespace agent::process;

namespace {
/// 辅助：判断给定的路径（如果存在）是否是绝对路径
bool is_absolute_if_present(const std::optional<std::string>& p) {
    if (!p) return true;  // nullopt 视为"契约满足"
    return std::filesystem::path(*p).is_absolute();
}
}  // namespace

// ============================================================
// resolve_ripgrep 基本契约
// ============================================================

TEST_CASE("ToolRegistry resolve_ripgrep returns absolute path or nullopt",
          "[tool_registry][ripgrep]") {
    auto& reg = ToolRegistry::instance();
    reg.clear_cache();  // 清缓存确保重新探测

    auto path = reg.resolve_ripgrep();
    // 要么返回 nullopt（未安装），要么返回绝对路径
    REQUIRE(is_absolute_if_present(path));

    // 如果返回了路径，文件应该存在
    if (path) {
        REQUIRE(std::filesystem::exists(*path));
    }
}

// ============================================================
// 缓存行为
// ============================================================

TEST_CASE("ToolRegistry caches resolve result", "[tool_registry][cache]") {
    auto& reg = ToolRegistry::instance();
    reg.clear_cache();

    auto first = reg.resolve_ripgrep();
    auto second = reg.resolve_ripgrep();

    // 两次结果应一致（要么都是 nullopt，要么都是相同路径）
    REQUIRE(first.has_value() == second.has_value());
    if (first) {
        REQUIRE(*first == *second);
    }
}

// ============================================================
// clear_cache
// ============================================================

TEST_CASE("ToolRegistry clear_cache allows re-resolve", "[tool_registry][cache]") {
    auto& reg = ToolRegistry::instance();
    reg.clear_cache();
    auto first = reg.resolve_ripgrep();

    reg.clear_cache();
    auto second = reg.resolve_ripgrep();

    // clear 后重新探测，结果应一致（环境未变）
    REQUIRE(first.has_value() == second.has_value());
    if (first) {
        REQUIRE(*first == *second);
    }
}

// ============================================================
// resolve_tool 通用接口
// ============================================================

TEST_CASE("ToolRegistry resolve_tool for nonexistent tool returns nullopt",
          "[tool_registry][resolve]") {
    auto& reg = ToolRegistry::instance();
    // 查找一个一定不存在的工具
    auto path = reg.resolve_tool("nonexistent_tool_xyz_999", "tools/nonexistent_xyz_999.exe",
                                 "nonexistent_xyz_999_cmd");
    REQUIRE_FALSE(path.has_value());
}

// ============================================================
// resolve_jq 基本契约（bundled <exe_dir>/tools/jq > PATH）
// ============================================================

TEST_CASE("ToolRegistry resolve_jq returns absolute path or nullopt", "[tool_registry][jq]") {
    auto& reg = ToolRegistry::instance();
    reg.clear_cache();

    auto path = reg.resolve_jq();
    // 要么返回 nullopt（未捆绑且未安装），要么返回绝对路径
    REQUIRE(is_absolute_if_present(path));

    // 如果返回了路径，文件应该存在
    if (path) {
        REQUIRE(std::filesystem::exists(*path));
    }
}

TEST_CASE("ToolRegistry resolve_jq result is cached consistently", "[tool_registry][jq]") {
    auto& reg = ToolRegistry::instance();
    reg.clear_cache();

    auto first = reg.resolve_jq();
    auto second = reg.resolve_jq();
    REQUIRE(first.has_value() == second.has_value());
    if (first) {
        REQUIRE(*first == *second);
    }
}
