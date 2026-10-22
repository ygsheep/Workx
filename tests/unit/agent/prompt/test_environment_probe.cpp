/**
 * @file test_environment_probe.cpp
 * @brief 环境上下文探测单元测试（Issue #82）
 * @details 只测**纯函数**部分：目录骨架（文件扫描）与项目命令推导（读标记文件），
 *          二者都不起子进程、不依赖真实机器环境，因此结果确定。
 *          工具链探测要 spawn `--version`，结果随机器而变，这里只断言结构不变量。
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/verdict.h"
#include "agent/prompt/environment_probe.h"

namespace {

using agent::AgentGoal;
using agent::detect_goal_command;
using agent::prompt::EnvironmentProbe;
using agent::prompt::format_environment_probe;
using agent::prompt::probe_directory_skeleton;
using agent::prompt::probe_project_commands;
using agent::prompt::probe_toolchain;
using agent::prompt::ProjectCommands;

namespace fs = std::filesystem;

/// 独占临时目录（析构时清理）
class TempDir {
   public:
    explicit TempDir(const std::string& name)
        : path_(fs::temp_directory_path() / ("workx_probe_" + name)) {
        std::error_code ec;
        fs::remove_all(path_, ec);
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }

    void write(const std::string& rel, const std::string& content) const {
        const fs::path target = path_ / rel;
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary);
        out << content;
    }

   private:
    fs::path path_;
};

/// 骨架里是否出现过某行
bool has_line(const std::vector<std::string>& lines, const std::string& want) {
    for (const auto& line : lines) {
        if (line == want) return true;
    }
    return false;
}

/// 第一个文件条目在骨架中的下标（找不到返回 npos）
size_t first_file_index(const std::vector<std::string>& lines) {
    for (size_t i = 0; i < lines.size(); ++i) {
        if (!lines[i].starts_with("  ") && !lines[i].ends_with("/")) return i;
    }
    return std::string::npos;
}

/// 造一个含噪音目录的项目骨架
std::vector<std::string> skeleton_with_noise() {
    TempDir dir("noise");
    dir.write("build/CMakeCache.txt", "# cache\n");
    dir.write(".git/config", "[core]\n");
    dir.write("node_modules/pkg/index.js", "module.exports = 1;\n");
    dir.write("src/main.cpp", "int main() {}\n");
    dir.write("top_file.txt", "x");
    return probe_directory_skeleton(dir.path());
}

/// 造一个带 preset 构建目录的 CMake 项目
ProjectCommands preset_project() {
    TempDir dir("cmake_preset");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    dir.write("CMakePresets.json", R"({"version":3,"configurePresets":[{"name":"default",)"
                                   R"("binaryDir":"${sourceDir}/out"}]})");
    dir.write("out/CMakeCache.txt", "# cache\n");
    return probe_project_commands(dir.path());
}

/// 造一个已 configure 过、构建目录在 build/ 的 CMake 项目
ProjectCommands cached_project() {
    TempDir dir("cmake_cache");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    dir.write("build/CMakeCache.txt", "# cache\n");
    return probe_project_commands(dir.path());
}

/// 造一个只有 CMakeLists.txt 的裸项目（从未 configure 过）
ProjectCommands bootstrap_project() {
    TempDir dir("cmake_bootstrap");
    dir.write("CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    return probe_project_commands(dir.path());
}

/// 造一个 Node 项目
ProjectCommands node_project() {
    TempDir dir("node");
    dir.write("package.json",
              R"({"name":"x","scripts":{"build":"tsc","test":"jest","lint":"eslint ."}})");
    return probe_project_commands(dir.path());
}

}  // namespace

TEST_CASE("目录骨架跳过构建产物与依赖目录", "[issue82][environment_probe]") {
    const auto lines = skeleton_with_noise();

    CHECK(has_line(lines, "src/"));
    CHECK(has_line(lines, "  main.cpp"));
    CHECK_FALSE(has_line(lines, "build/"));
    CHECK_FALSE(has_line(lines, ".git/"));
    CHECK_FALSE(has_line(lines, "node_modules/"));
}

TEST_CASE("目录骨架把目录排在前、子层缩进两格", "[issue82][environment_probe]") {
    const auto lines = skeleton_with_noise();

    const size_t dir_index = [&lines] {
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i] == "src/") return i;
        }
        return std::string::npos;
    }();
    REQUIRE(dir_index != std::string::npos);
    REQUIRE(first_file_index(lines) != std::string::npos);
    CHECK(dir_index < first_file_index(lines));  // 目录在前
    CHECK(has_line(lines, "  main.cpp"));        // 子层缩进
}

TEST_CASE("目录骨架有总行数上限，不会撑爆提示词", "[issue82][environment_probe]") {
    TempDir dir("limit");
    for (int i = 0; i < 120; ++i) {
        dir.write("file_" + std::to_string(i) + ".txt", "x");
    }
    const auto lines = probe_directory_skeleton(dir.path());

    CHECK_FALSE(lines.empty());
    CHECK(lines.size() <= 80u);
}

TEST_CASE("CMake 构建命令取 preset 里的真实构建目录", "[issue82][environment_probe]") {
    CHECK(preset_project().build == "cmake --build ./out --config Debug");
}

TEST_CASE("CMake 无 presets 时按已生成的缓存取 build/", "[issue82][environment_probe]") {
    CHECK(cached_project().build == "cmake --build build --config Debug");
}

TEST_CASE("CMake 从未配置过则给自举命令", "[issue82][environment_probe]") {
    CHECK(bootstrap_project().build == "cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug");
}

TEST_CASE("Node 项目的 test / build 来自项目标记文件", "[issue82][environment_probe]") {
    const auto c = node_project();

    CHECK(c.test == "npm test");
    CHECK(c.build == "npm run build");
    // package.json 里明明有 lint script，但 #78 的 LintZero 只认 eslint 配置文件。
    // 这是「与门禁同源」的代价：宁可少给，也不给一条门禁根本不会跑的命令。
    CHECK(c.lint.empty());
}

TEST_CASE("无已知标记文件时不推导任何命令", "[issue82][environment_probe]") {
    TempDir dir("unknown");
    dir.write("main.cpp", "int main() {}\n");

    const auto c = probe_project_commands(dir.path());

    CHECK(c.build.empty());
    CHECK(c.test.empty());
    CHECK(c.lint.empty());
}

TEST_CASE("test / lint 命令与验证门禁同源（#78 detect_goal_command）",
          "[issue82][environment_probe]") {
    struct Shape {
        const char* name;
        std::vector<std::pair<std::string, std::string>> files;
    };
    const Shape shapes[] = {
        {"cmake",
         {{"CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n"},
          {"CTestTestfile.cmake", "# ctest\n"}}},
        {"node", {{"package.json", R"({"scripts":{"test":"jest"}})"}}},
        {"make", {{"Makefile", "test:\n\t./run_tests\n"}}},
        {"cargo", {{"Cargo.toml", "[package]\n"}}},
        {"go", {{"go.mod", "module x\n"}}},
    };

    for (const auto& shape : shapes) {
        TempDir dir(std::string("same_src_") + shape.name);
        for (const auto& [rel, content] : shape.files) {
            dir.write(rel, content);
        }
        const auto c = probe_project_commands(dir.path());
        // 提示词给模型的 == 门禁实际会跑的。两边一旦漂移，用例立刻红。
        CHECK(c.test == detect_goal_command(AgentGoal::TestsPass, dir.path().string()));
        CHECK(c.lint == detect_goal_command(AgentGoal::LintZero, dir.path().string()));
    }
}

TEST_CASE("渲染：全部为空时返回空串", "[issue82][environment_probe]") {
    CHECK(format_environment_probe(EnvironmentProbe{}).empty());
}

TEST_CASE("渲染：非空时给出三个小节并提示优先使用", "[issue82][environment_probe]") {
    EnvironmentProbe probe;
    probe.dir_entries = {"src/", "  main.cpp"};
    probe.toolchains = {{.name = "cmake", .found = true, .version = "3.28.1"},
                        {.name = "rg", .found = false, .version = ""}};
    probe.commands = {.build = "cmake --build build", .test = "ctest", .lint = {}};

    const std::string text = format_environment_probe(probe);

    CHECK(text.find("## Project layout") != std::string::npos);
    CHECK(text.find("## Available toolchain") != std::string::npos);
    CHECK(text.find("- cmake 3.28.1") != std::string::npos);
    CHECK(text.find("- rg") == std::string::npos);  // 未安装的不列
    CHECK(text.find("## Build & test commands (detected from project files)") != std::string::npos);
    CHECK(text.find("Use these instead of guessing") != std::string::npos);
}

TEST_CASE("工具链条目固定且顺序稳定", "[issue82][environment_probe]") {
    const auto tools = probe_toolchain();

    REQUIRE(tools.size() == 8u);
    CHECK(tools[0].name == "cmake");
    CHECK(tools[1].name == "ctest");
    CHECK(tools.back().name == "rg");
    for (const auto& tool : tools) {
        CHECK_FALSE(tool.name.empty());
    }
}
