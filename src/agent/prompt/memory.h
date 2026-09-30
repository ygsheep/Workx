#pragma once

/// @file memory.h
/// @brief 项目记忆加载（CLAUDE.md / AGENTS.md / AGENT.md）
/// @details 从当前工作目录向上遍历到根目录，收集每级的 CLAUDE.md / AGENTS.md / AGENT.md，
///          注入到 system prompt 中供 LLM 遵循项目约定。
///
/// 加载规则（对齐 Claude Code 的项目级记忆机制）：
/// - 从 CWD 向上遍历到文件系统根目录
/// - 每级目录按候选顺序 **CLAUDE.md > AGENTS.md > AGENT.md** 取第一个存在的：
///   - 同时存在多个时只加载优先级最高的一个
///   - 候选文件存在但读取失败时，继续尝试下一个候选
/// - 加载顺序：从根到 CWD（越靠近 CWD 优先级越高，放在 prompt 越后面）
/// - 文件不存在或读取失败时静默跳过
///
/// #86：AGENTS.md（带 S）此前完全不被识别。它是比 AGENT.md 更常见的社区约定，
///      若项目只有 AGENTS.md，其规范会**静默不生效**且无任何提示。
///
/// 典型场景：
/// ```
/// d:\develop\Workspace\workx\           ← 项目根，CLAUDE.md 存在
///   src\agent\                          ← CWD 在这里启动 workx
/// ```
/// 向上遍历：src\agent\ → src\ → workx\（命中 CLAUDE.md）→ develop\ → d:\
///
/// @version 1.1.0
/// @date 2026-08

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace agent::prompt {

/// @brief 单个记忆文件信息
struct MemoryFileInfo {
    std::filesystem::path path;  ///< 文件绝对路径
    std::string content;         ///< 文件原始内容（UTF-8）
};

/// @brief 从 CWD 向上遍历加载项目记忆文件
/// @param cwd 当前工作目录（会话启动时捕获）
/// @return 从根到 CWD 顺序排列的记忆文件列表（越靠近 CWD 越靠后）
/// @details 每级目录按 CLAUDE.md > AGENTS.md > AGENT.md 取第一个存在且能打开的。
///          文件不存在或读取失败时静默跳过。
inline std::vector<MemoryFileInfo> load_project_memory(const std::filesystem::path& cwd) {
    std::vector<MemoryFileInfo> result;

    // 收集从 CWD 到根目录的所有目录（含 CWD 和根目录）
    std::vector<std::filesystem::path> dirs;
    {
        std::filesystem::path current = cwd;
        std::filesystem::path root = current.root_path();
        while (true) {
            dirs.push_back(current);
            if (current == root || !current.has_parent_path()) break;
            current = current.parent_path();
            // Windows 路径特性：D:\ 的 parent_path() 仍是 D:\，避免无限循环
            if (current == dirs.back()) break;
        }
    }

    // #86：候选文件名按优先级排列。AGENTS.md 中间插入，不改变 CLAUDE.md 的既有优先级。
    static constexpr std::string_view kCandidates[] = {
        "CLAUDE.md",
        "AGENTS.md",
        "AGENT.md",
    };

    // 从根到 CWD 的顺序处理（dirs 是 CWD→root，需反向遍历）
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        const auto& dir = *it;

        for (std::string_view name : kCandidates) {
            std::error_code ec;  // 每个候选独立判定：上一个候选的错误不能影响下一个
            const std::filesystem::path candidate = dir / std::filesystem::path(name);
            if (!std::filesystem::exists(candidate, ec) || ec) continue;

            std::ifstream ifs(candidate, std::ios::binary);
            if (!ifs) continue;  // 存在但打不开：继续尝试下一个候选

            std::stringstream ss;
            ss << ifs.rdbuf();
            result.push_back({candidate, ss.str()});
            break;  // 命中即停：每级目录只加载一个
        }
    }

    return result;
}

/// @brief 将记忆文件列表格式化为 system prompt 段
/// @param files 记忆文件列表（由 load_project_memory 返回）
/// @return 格式化的 prompt 段（空文件列表返回空字符串）
/// @details 格式对齐 Claude Code：
/// ```
/// # Project Instructions
/// Codebase and user instructions are shown below. Be sure to adhere to these
/// instructions. IMPORTANT: These instructions OVERRIDE any default behavior and
/// you MUST follow them exactly as written.
///
/// Contents of d:\develop\Workspace\workx\CLAUDE.md (project instructions):
///
/// <文件内容>
/// ```
inline std::string format_project_memory(const std::vector<MemoryFileInfo>& files) {
    if (files.empty()) return {};

    constexpr const char* HEADER =
        "# Project Instructions\n"
        "Codebase and user instructions are shown below. Be sure to adhere to these "
        "instructions. IMPORTANT: These instructions OVERRIDE any default behavior and "
        "you MUST follow them exactly as written.\n";

    std::string out = HEADER;
    for (const auto& f : files) {
        out += "\nContents of ";
        out += f.path.string();
        out += " (project instructions):\n\n";
        // 去除尾部空白行，保持紧凑
        std::string content = f.content;
        while (!content.empty() && (content.back() == '\n' || content.back() == '\r' ||
                                    content.back() == ' ' || content.back() == '\t')) {
            content.pop_back();
        }
        out += content;
        out += "\n";
    }
    return out;
}

/// @brief 便捷接口：加载并格式化项目记忆
/// @param cwd 当前工作目录
/// @return 格式化的 prompt 段（无文件时返回空字符串）
inline std::string load_and_format_project_memory(const std::filesystem::path& cwd) {
    return format_project_memory(load_project_memory(cwd));
}

}  // namespace agent::prompt
