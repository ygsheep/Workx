/**
 * @file hooks.h
 * @brief Skill PreActivate 钩子执行器
 * @details 执行 SKILL.md frontmatter `hooks` 字段声明的命令（激活 skill 前运行）。
 *          复用 core/process 的跨平台子进程封装：超时、输出捕获、取消支持。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

namespace agent::skill {

/// @brief 执行 PreActivate 钩子命令
/// @param hooks 命令列表（frontmatter hooks 字段，原样交给 shell 解释）
/// @param cwd 工作目录
/// @param timeout_ms 单条命令超时（毫秒），默认 30s
/// @return 每条命令一行输出：`[ok] <cmd>\n<截断输出>` 或 `[fail] <cmd>: <错误>`；
///         单条失败不中断后续钩子
std::vector<std::string> run_preactivate_hooks(const std::vector<std::string>& hooks,
                                               const std::string& cwd, int timeout_ms = 30000);

/// @brief 钩子输出拼接为单块文本（注入激活前缀用）
std::string format_hook_output(const std::vector<std::string>& lines);

}  // namespace agent::skill
