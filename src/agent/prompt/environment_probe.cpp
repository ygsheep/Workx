/**
 * @file environment_probe.cpp
 * @brief 环境上下文探测实现（Issue #82）
 */

#include "agent/prompt/environment_probe.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <optional>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agent/core/verdict.h"  // #78 P4：detect_goal_command()（命令单一来源）
#include "core/process/subprocess.h"

namespace agent::prompt {
namespace {

// ============================================================
// 目录骨架
// ============================================================

constexpr size_t kMaxTopEntries = 40;       ///< 顶层条目上限
constexpr size_t kMaxChildEntries = 10;     ///< 每个子目录展开的条目上限
constexpr size_t kMaxSkeletonEntries = 80;  ///< 骨架总行数上限（防止撑爆提示词）

/// @brief 是否应跳过的噪音目录（构建产物 / VCS / 依赖目录）
bool is_noise_dir(std::string_view name) {
    static constexpr std::string_view kNoise[] = {
        ".git",        ".svn",         ".hg",         ".vs",
        ".idea",       ".vscode",      ".cache",      "build",
        "out",         "dist",         "target",      "bin",
        "obj",         "node_modules", "__pycache__", "vcpkg_installed",
        "third_party",
    };
    for (std::string_view n : kNoise) {
        if (name == n) return true;
    }
    return name.starts_with("build_");  // build_probe6/ 这类一次性构建目录
}

struct Entry {
    std::string name;
    bool is_dir = false;
};

/// @brief 列出目录下的条目（目录排在前，各自按名字排序，截断到 limit）
std::vector<Entry> list_dir(const std::filesystem::path& dir, size_t limit) {
    std::vector<Entry> entries;
    std::error_code ec;
    for (const auto& item : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        const bool is_dir = item.is_directory(ec);
        if (!is_dir && !item.is_regular_file(ec)) continue;
        if (is_dir && is_noise_dir(item.path().filename().string())) continue;
        entries.push_back({item.path().filename().string(), is_dir});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.is_dir != b.is_dir) return a.is_dir;  // 目录在前
        return a.name < b.name;
    });
    if (entries.size() > limit) entries.resize(limit);
    return entries;
}

// ============================================================
// 工具链
// ============================================================

constexpr std::string_view kToolNames[] = {"cmake", "ctest", "python", "python3",
                                           "npm",   "cargo", "go",     "rg"};
constexpr auto kVersionTimeout = std::chrono::milliseconds(1200);

/// @brief 在 PATH 中查找可执行文件（不起子进程，纯文件系统扫描）
std::optional<std::filesystem::path> find_on_path(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (path_env == nullptr) return std::nullopt;
#if defined(_WIN32)
    constexpr char kSep = ';';
#else
    constexpr char kSep = ':';
#endif
    std::string_view rest(path_env);
    std::error_code ec;
    while (!rest.empty()) {
        const auto pos = rest.find(kSep);
        const auto dir = (pos == std::string_view::npos) ? rest : rest.substr(0, pos);
        if (!dir.empty()) {
            for (const char* suffix : {"", ".exe"}) {
                const auto candidate = std::filesystem::path(dir) / (std::string(name) + suffix);
                if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
            }
        }
        if (pos == std::string_view::npos) break;
        rest.remove_prefix(pos + 1);
    }
    return std::nullopt;
}

/// @brief 取 `--version` 输出的首行（stdout 优先，退回 stderr）
/// @param exe 已解析到的可执行文件路径
std::string query_version(const std::filesystem::path& exe) {
    try {
        // 逐字段赋值而非聚合初始化：ExecOptions 有多数场景用不到的成员
        // （cwd / is_cancelled / isolation），聚合初始化会触发 GCC 的
        // -Wmissing-field-initializers。
        process::ExecOptions opts;
        opts.args = {"--version"};
        opts.timeout = kVersionTimeout;
        opts.stdin_mode = process::StdinMode::Null;
        auto res = process::exec(exe.string(), opts);
        if (res.is_ok()) {
            std::string text = res.value().stdout_text;
            if (text.empty()) text = res.value().stderr_text;
            const auto nl = text.find('\n');
            if (nl != std::string::npos) text.resize(nl);
            while (!text.empty() && text.back() == '\r') text.pop_back();
            return text;
        }
    } catch (...) {
        // 版本探测属于锦上添花：失败不影响「工具存在」这一结论
    }
    return {};
}

// ============================================================
// 项目命令推导
// ============================================================

/// @brief 文件是否存在（不抛异常）
bool has_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

/// @brief 读文件全部内容，失败返回空串
std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// @brief 从 CMakePresets.json 取构建目录
/// @details 只取第一个 configurePreset 的 binaryDir；`${sourceDir}` 展开为 "."。
///          解析失败一律回退 nullopt，由调用方走更粗的推导。
std::optional<std::string> preset_binary_dir(const std::filesystem::path& cwd) {
    const std::string text = read_file(cwd / "CMakePresets.json");
    if (text.empty()) return std::nullopt;
    try {
        const auto j = nlohmann::json::parse(text);
        const auto presets = j.find("configurePresets");
        if (presets == j.end() || !presets->is_array() || presets->empty()) return std::nullopt;
        const auto dir = presets->at(0).find("binaryDir");
        if (dir == presets->at(0).end() || !dir->is_string()) return std::nullopt;
        std::string out = dir->get<std::string>();
        constexpr std::string_view kToken = "${sourceDir}";
        for (size_t pos = out.find(kToken); pos != std::string::npos; pos = out.find(kToken)) {
            out.replace(pos, kToken.size(), ".");
        }
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief 推导 CMake 项目的构建命令
/// @details #78 的 BuildClean 对 CMake 是硬编码的 `cmake --build .`，对构建目录
///          不在 CWD 的项目（本仓就是 build/）必然失败。故这里按 presets 或已
///          生成的缓存拿真实构建目录；从未配置过时给一条能自举的命令。
/// @return 构建命令；不是 CMake 项目返回 nullopt
std::optional<std::string> detect_cmake_build(const std::filesystem::path& cwd) {
    std::string binary_dir;
    if (const auto preset = preset_binary_dir(cwd)) {
        binary_dir = *preset;
    } else if (has_file(cwd / "build" / "CMakeCache.txt")) {
        binary_dir = "build";
    } else if (has_file(cwd / "CMakeLists.txt")) {
        return std::string("cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug");
    } else {
        return std::nullopt;
    }
    return std::format("cmake --build {} --config Debug", binary_dir);
}

}  // namespace

std::vector<std::string> probe_directory_skeleton(const std::filesystem::path& cwd) {
    std::vector<std::string> out;
    for (const auto& entry : list_dir(cwd, kMaxTopEntries)) {
        if (out.size() >= kMaxSkeletonEntries) break;
        out.push_back(entry.is_dir ? entry.name + "/" : entry.name);
        if (!entry.is_dir) continue;
        for (const auto& child : list_dir(cwd / entry.name, kMaxChildEntries)) {
            if (out.size() >= kMaxSkeletonEntries) break;
            out.push_back("  " + child.name + (child.is_dir ? "/" : ""));
        }
    }
    return out;
}

std::vector<ToolchainInfo> probe_toolchain() {
    std::vector<ToolchainInfo> out;
    out.reserve(std::size(kToolNames));
    for (std::string_view name : kToolNames) {
        ToolchainInfo info;
        info.name = std::string(name);
        if (const auto exe = find_on_path(name)) {
            info.found = true;
            info.version = query_version(*exe);
        }
        out.push_back(std::move(info));
    }
    return out;
}

ProjectCommands probe_project_commands(const std::filesystem::path& cwd) {
    ProjectCommands out;
    // test / lint 直接复用 #78 P4 的探测结果（detect_goal_command）：
    // 提示词告诉模型的命令必须与验证门禁实际会执行的命令同源，否则模型按提示
    // 跑一遍、门禁又按自己的口径跑一遍，两边结论还可能互相打脸。
    out.test = detect_goal_command(AgentGoal::TestsPass, cwd.string());
    out.lint = detect_goal_command(AgentGoal::LintZero, cwd.string());

    // build 只补 #78 拿不到的那部分：CMake 的真实构建目录（见 detect_cmake_build）。
    // 非 CMake 项目交给 #78 的 BuildClean 探测（cargo / go / npm / make）。
    if (const auto cmake_build = detect_cmake_build(cwd)) {
        out.build = *cmake_build;
    } else {
        out.build = detect_goal_command(AgentGoal::BuildClean, cwd.string());
    }
    return out;
}

std::string format_environment_probe(const EnvironmentProbe& probe) {
    std::string out;
    if (!probe.dir_entries.empty()) {
        out += "## Project layout (top 2 levels)\n";
        for (const auto& line : probe.dir_entries) {
            out += line + "\n";
        }
    }

    std::string tools;
    for (const auto& tool : probe.toolchains) {
        if (!tool.found) continue;
        tools += std::format("- {}{}\n", tool.name, tool.version.empty() ? "" : " " + tool.version);
    }
    if (!tools.empty()) {
        if (!out.empty()) out += "\n";
        out += "## Available toolchain\n" + tools;
    }

    const ProjectCommands& c = probe.commands;
    if (!c.build.empty() || !c.test.empty() || !c.lint.empty()) {
        if (!out.empty()) out += "\n";
        out += "## Build & test commands (detected from project files)\n";
        if (!c.build.empty()) out += std::format("- Build: `{}`\n", c.build);
        if (!c.test.empty()) out += std::format("- Test: `{}`\n", c.test);
        if (!c.lint.empty()) out += std::format("- Lint: `{}`\n", c.lint);
        out +=
            "Use these instead of guessing; run the test command before claiming a task is "
            "done.\n";
    }
    return out;
}

}  // namespace agent::prompt
