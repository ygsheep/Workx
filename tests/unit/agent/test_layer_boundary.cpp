/**
 * @file test_layer_boundary.cpp
 * @brief 分层边界编译期校验（Issue #21）
 * @details 扫描 src/agent 与 src/core 下所有 .h/.cpp，断言无反向
 *          include 上层（tui/app/example）：
 *          - agent 层禁止 include tui/app/example
 *          - core 层禁止 include agent/tui/app/example
 *          防止分层被破坏（libworkx 库化改造后此约束即 ABI 边界承诺）。
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct ForbiddenInclude {
    const char* pattern;
    const char* dir;    ///< 被扫描层
    const char* owner;  ///< 禁止 include 的目标层
};

/// @brief 读取文件为字符串（按二进制读，避免编码问题）
std::string read_file_binary(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/// @brief 检查单条禁止规则
/// @return 违规文件路径列表（空 = 通过）
std::vector<fs::path> check_rule(const fs::path& layer_dir, const char* forbidden) {
    std::vector<fs::path> violations;
    const std::string needle = std::string("#include \"") + forbidden;

    for (const auto& entry : fs::recursive_directory_iterator(layer_dir)) {
        if (!entry.is_regular_file()) continue;
        const auto& p = entry.path();
        const auto ext = p.extension().string();
        if (ext != ".h" && ext != ".hpp" && ext != ".cpp") continue;
        if (read_file_binary(p).find(needle) != std::string::npos) {
            violations.push_back(p);
        }
    }
    return violations;
}

}  // namespace

// ============================================================
// agent 层：禁止 include tui/app/example
// ============================================================

TEST_CASE("agent layer must not include tui/app/example", "[layer_boundary]") {
    const fs::path agent_dir = fs::path(SOURCE_DIR) / "src" / "agent";

    for (const char* forbidden : {"tui/", "app/", "example/"}) {
        auto violations = check_rule(agent_dir, forbidden);
        INFO("agent 层禁止 include \"" << forbidden << "\"，违规文件：");
        for (const auto& v : violations) {
            INFO("  " << v.string());
        }
        REQUIRE(violations.empty());
    }
}

// ============================================================
// core 层：禁止 include agent/tui/app/example
// ============================================================

TEST_CASE("core layer must not include agent/tui/app/example", "[layer_boundary]") {
    const fs::path core_dir = fs::path(SOURCE_DIR) / "src" / "core";

    for (const char* forbidden : {"agent/", "tui/", "app/", "example/"}) {
        auto violations = check_rule(core_dir, forbidden);
        INFO("core 层禁止 include \"" << forbidden << "\"，违规文件：");
        for (const auto& v : violations) {
            INFO("  " << v.string());
        }
        REQUIRE(violations.empty());
    }
}
