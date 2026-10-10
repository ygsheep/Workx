/**
 * @file environment_probe.h
 * @brief 环境上下文探测（Issue #82）
 * @details 启动时一次性采集「这个项目是什么、机器上有什么工具链、该怎么构建和测试」，
 *          供系统提示注入。解决 agent 每次都要靠 Bash + Glob 自己摸索环境、
 *          白白消耗前若干轮的问题（LangChain 的 LocalContextMiddleware 即此意）。
 *
 *          三类信息：
 *          1. 目录骨架 —— CWD 下 1~2 层，跳过 build/ node_modules/ .git/ 等噪音目录
 *          2. 工具链 —— cmake/ctest/python/python3/npm/cargo/go/rg 的存在性与版本
 *          3. 项目命令 —— 从 CMakePresets.json / build/CMakeCache.txt / package.json /
 *                        Makefile / Cargo.toml / go.mod 推导构建与测试命令
 *
 * @note 只做**只读**探测：不创建文件、不执行构建。版本探测会起子进程，
 *       但只对「已在 PATH 中找到」的工具发起（未安装的工具不会被 spawn），
 *       并带 1200ms 超时，避免拖慢启动。
 */

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent::prompt {

/// @brief 单个工具链的探测结果
struct ToolchainInfo {
    std::string name;     ///< 工具名（如 "cmake"）
    bool found = false;   ///< 是否在 PATH 中找到
    std::string version;  ///< 版本首行（可能为空：工具存在但取不到版本）
};

/// @brief 从项目标记文件推导出的命令
struct ProjectCommands {
    std::string build;  ///< 构建命令（空 = 推导不出）
    std::string test;   ///< 测试命令（空 = 推导不出）
    std::string lint;   ///< lint 命令（空 = 无）
};

/// @brief 一次完整的环境探测结果
struct EnvironmentProbe {
    std::vector<std::string> dir_entries;  ///< 目录骨架行（已缩进、已排序、有上限）
    std::vector<ToolchainInfo> toolchains;  ///< 工具链（固定顺序，未安装也占一项）
    ProjectCommands commands;               ///< 项目命令
};

/// @brief 探测 CWD 下的目录骨架（1~2 层，跳过噪音目录，结果有序且有条数上限）
/// @param cwd 起始目录；不存在或不可读时返回空
/// @return 每行一条，目录以 '/' 结尾，子层缩进两个空格
std::vector<std::string> probe_directory_skeleton(const std::filesystem::path& cwd);

/// @brief 探测工具链的存在性与版本
/// @details 存在性走 PATH 扫描（不起进程）；版本只对已找到的工具发起子进程，
///          单条 1200ms 超时。任一环节失败都不影响其余工具。
std::vector<ToolchainInfo> probe_toolchain();

/// @brief 从项目标记文件推导构建 / 测试 / lint 命令
/// @details test 与 lint 直接取 #78 P4 的 `detect_goal_command()` —— 提示词给的
///          命令必须与验证门禁实际执行的命令同源，否则模型按提示跑一遍、门禁按
///          自己的口径再跑一遍，两边结论还可能互相打脸。
///          build 在此基础上补 #78 拿不到的 CMake 真实构建目录
///          （#78 的 `cmake --build .` 对构建目录不在 CWD 的项目必然失败）。
/// @param cwd 项目根目录
/// @return 推导不出时对应字段为空
ProjectCommands probe_project_commands(const std::filesystem::path& cwd);

/// @brief 推导 CMake 项目的构建目录（不含自举）
/// @details #129：presets 的 binaryDir（${sourceDir} 已展开为相对路径）优先，
///          其次 build/CMakeCache.txt（out-of-source 惯例），再次 CWD 下的
///          CMakeCache.txt（in-source，返回 "."）。供验证门禁（verdict.cpp）
///          与提示词侧共用同一口径——门禁实际执行的命令必须与提示词给模型
///          看的命令指向同一个构建目录。
/// @param cwd 项目根目录
/// @return 构建目录（相对 cwd 或 presets 给出的原始路径）；推导不出返回 nullopt
std::optional<std::string> detect_cmake_binary_dir(const std::filesystem::path& cwd);

/// @brief 推导 CMake 项目的构建命令（构建目录指向真实 binary dir）
/// @details #129：presets / build 缓存 / in-source 缓存推导出构建目录后给
///          `cmake --build <dir> --config Debug`；目录只是声明了但从未
///          configure（无 CMakeCache.txt）或整个项目未 configure 时，给一条
///          能自举的 `cmake -S . -B build ...`——绝不返回必然失败的命令。
///          验证门禁（verdict.cpp 的 BuildClean 探测）与提示词侧共用。
/// @param cwd 项目根目录
/// @return 构建命令；不是 CMake 项目返回 nullopt
std::optional<std::string> detect_cmake_build(const std::filesystem::path& cwd);

/// @brief 把探测结果渲染为注入系统提示的文本块
/// @param probe 探测结果
/// @return 渲染文本；**全部为空时返回空串**（调用方据此决定是否追加）
std::string format_environment_probe(const EnvironmentProbe& probe);

}  // namespace agent::prompt
