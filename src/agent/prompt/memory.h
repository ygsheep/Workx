#pragma once

/// @file memory.h
/// @brief 项目记忆加载（CLAUDE.md / AGENT.md）
/// @details 从当前工作目录向上遍历到根目录，收集每级的 CLAUDE.md 或 AGENT.md，
///          注入到 system prompt 中供 LLM 遵循项目约定。
///
/// 加载规则（对齐 Claude Code 的项目级记忆机制）：
/// - 从 CWD 向上遍历到文件系统根目录
/// - 每级目录查找 CLAUDE.md 和 AGENT.md，**二选一**：
///   - 同时存在时优先加载 CLAUDE.md，不加载 AGENT.md
///   - 只有 AGENT.md 时加载 AGENT.md
/// - 加载顺序：从根到 CWD（越靠近 CWD 优先级越高，放在 prompt 越后面）
/// - 文件不存在或读取失败时静默跳过
///
/// 典型场景：
/// ```
/// d:\develop\Workspace\workx\           ← 项目根，CLAUDE.md 存在
///   src\agent\                          ← CWD 在这里启动 workx
/// ```
/// 向上遍历：src\agent\ → src\ → workx\（命中 CLAUDE.md）→ develop\ → d:\
///
/// @version 1.0.0
/// @date 2026-08

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace agent::prompt {

/// @brief 单个记忆文件信息
struct MemoryFileInfo {
    std::filesystem::path path;  ///< 文件绝对路径
    std::string content;         ///< 文件原始内容（UTF-8）
};

/// @brief 从 CWD 向上遍历加载 CLAUDE.md / AGENT.md
/// @param cwd 当前工作目录（会话启动时捕获）
/// @return 从根到 CWD 顺序排列的记忆文件列表（越靠近 CWD 越靠后）
/// @details 每级目录二选一：CLAUDE.md 优先于 AGENT.md。
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

    // 从根到 CWD 的顺序处理（dirs 是 CWD→root，需反向遍历）
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        const auto& dir = *it;
        std::error_code ec;

        // 优先 CLAUDE.md
        std::filesystem::path claude_md = dir / "CLAUDE.md";
        if (std::filesystem::exists(claude_md, ec) && !ec) {
            std::ifstream ifs(claude_md, std::ios::binary);
            if (ifs) {
                std::stringstream ss;
                ss << ifs.rdbuf();
                result.push_back({claude_md, ss.str()});
                continue;  // CLAUDE.md 命中，跳过 AGENT.md
            }
        }

        // 次选 AGENT.md（仅当 CLAUDE.md 不存在或读取失败）
        std::filesystem::path agent_md = dir / "AGENT.md";
        if (std::filesystem::exists(agent_md, ec) && !ec) {
            std::ifstream ifs(agent_md, std::ios::binary);
            if (ifs) {
                std::stringstream ss;
                ss << ifs.rdbuf();
                result.push_back({agent_md, ss.str()});
            }
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
