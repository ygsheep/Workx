/**
 * @file test_layer_boundary.cpp
 * @brief island 层边界校验：island 只能 include core/agent，禁止 include
 *        上层（tui/app/example），保证未来 libworkx_island 库化的 ABI
 *        边界不被破坏。
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string read_file_binary(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/// @brief 检查单个禁止规则；返回违规文件列表（空 = 通过）
std::vector<fs::path> check_rule(const fs::path& layer_dir, const char* forbidden) {
    std::vector<fs::path> violations;
    const std::string needle = std::string("#include \"") + forbidden;
    for (const auto& entry : fs::recursive_directory_iterator(layer_dir)) {
        if (!entry.is_regular_file()) continue;
        const auto ext = entry.path().extension().string();
        if (ext != ".h" && ext != ".hpp" && ext != ".cpp") continue;
        if (read_file_binary(entry.path()).find(needle) != std::string::npos) {
            violations.push_back(entry.path());
        }
    }
    return violations;
}

}  // namespace

TEST_CASE("island layer must not include tui/app/example", "[layer_boundary]") {
    const fs::path island_dir = fs::path(SOURCE_DIR) / "src" / "island";
    for (const char* forbidden : {"tui/", "app/", "example/"}) {
        const auto violations = check_rule(island_dir, forbidden);
        INFO("island 层禁止 include \"" << forbidden << "\"，违规文件：");
        for (const auto& v : violations) {
            INFO("  " << v.string());
        }
        REQUIRE(violations.empty());
    }
}