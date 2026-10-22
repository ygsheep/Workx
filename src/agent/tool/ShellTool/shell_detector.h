/**
 * @file shell_detector.h
 * @brief Shell 检测器 — 运行期探测可用 shell（对齐 cc 的 BashTool 策略）
 * @details
 *   检测策略（按优先级）：
 *   - **非 Windows**：直接用 `/bin/sh`（POSIX 兼容）
 *   - **Windows**：
 *     1. 查找 Git Bash（`bash.exe`），找到则用 — 支持 Unix 命令（ls/grep/cat 等）
 *     2. 降级到 `cmd.exe` — 仅支持 Windows 命令（dir/findstr/type 等）
 *
 *   Git Bash 查找顺序：
 *   1. PATH 环境变量中的 `bash.exe`（用 Win32 SearchPathW）
 *   2. 常见安装路径（`C:\Program Files\Git\bin\bash.exe` 等）
 *   3. `%LOCALAPPDATA%\Programs\Git\bin\bash.exe`（用户级安装）
 *
 *   设计要点：
 *   - 首次调用后缓存，避免重复文件系统访问
 *   - 线程安全（C++11 magic statics）
 *   - BashTool 与 factory.cpp 共用，保证 env 段与实际 shell 一致
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace agent::tool::shell_detect {

/// @brief Shell 类型分类
enum class ShellType {
    UnixSh,   ///< 非 Windows 的 /bin/sh
    GitBash,  ///< Windows 上的 Git Bash（支持 Unix 命令）
    CmdExe,   ///< Windows cmd.exe（降级方案）
};

/// @brief Shell 探测结果
struct ShellInfo {
    std::string cmd;   ///< shell 可执行路径（绝对路径或 PATH 中的名字）
    std::string flag;  ///< 执行标志（"-c" 或 "/c"）
    ShellType type;    ///< shell 类型分类
    bool is_unix;      ///< 是否 Unix 风格（UnixSh 或 GitBash 为 true，CmdExe 为 false）
};

/// @brief 检测当前平台可用的 shell
/// @details 首次调用执行检测并缓存结果，后续调用零开销。线程安全。
/// @return ShellInfo 引用（静态对象，程序生命周期内有效）
const ShellInfo& detect();

}  // namespace agent::tool::shell_detect
