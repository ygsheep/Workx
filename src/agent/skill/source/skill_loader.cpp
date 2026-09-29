/**
 * @file skill_loader.cpp
 * @brief Skill 磁盘加载器实现
 * @details 目录遍历 + SKILL.md 解析 + PromptCommand 构建。
 *          仅支持 <name>/SKILL.md 目录格式（对齐 example/cc）。
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/skill/inclaude/skill_loader.h"
#include "agent/skill/inclaude/frontmatter.h"
#include "core/process/tool_registry.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace agent::skill {

std::vector<std::string> find_skill_dirs_up_to_home(const std::string& cwd) {
    std::vector<std::string> dirs;
    fs::path p = cwd;
    std::error_code ec;
    for (;;) {
        const auto candidate = p / ".claude" / "skills";
        if (fs::is_directory(candidate, ec) && !ec) {
            dirs.push_back(candidate.string());
        }
        ec.clear();
        const auto parent = p.parent_path();
        if (parent.empty() || parent == p) break;
        p = parent;
    }
    return dirs;
}

namespace {

/// @brief 获取用户 home 目录（Windows: USERPROFILE/HOMEDRIVE+HOMEPATH，POSIX: HOME）
std::string get_home_dir() {
#if defined(_WIN32)
    if (const char* p = std::getenv("USERPROFILE")) {
        if (p[0] != '\0') return p;
    }
    if (const char* drive = std::getenv("HOMEDRIVE")) {
        if (const char* path = std::getenv("HOMEPATH")) {
            return std::string(drive) + path;
        }
    }
#else
    if (const char* p = std::getenv("HOME")) {
        if (p[0] != '\0') return p;
    }
#endif
    return {};
}

}  // anonymous namespace

std::vector<std::string> find_user_skill_dirs() {
    const auto home = get_home_dir();
    if (home.empty()) return {};

    std::vector<std::string> dirs;
    std::error_code ec;
    for (const auto& sub : {".claude", ".workx"}) {
        const auto candidate = fs::path(home) / sub / "skills";
        if (fs::is_directory(candidate, ec) && !ec) {
            dirs.push_back(candidate.string());
        }
        ec.clear();
    }
    return dirs;
}

namespace {

/// @brief 读取文件内容；失败返回空 optional
std::optional<std::string> read_file_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return std::nullopt;
    return ss.str();
}

/// @brief 由解析结果构建 PromptCommand（含别名命令）
std::vector<std::shared_ptr<command::PromptCommand>> build_commands(const ParsedSkill& parsed,
                                                                    const fs::path& skill_dir) {
    const auto& fm = parsed.frontmatter;

    const auto gen = [body = parsed.body, base = skill_dir.string()](
                         const std::string& /*args*/, const command::CommandContext& /*ctx*/) {
        command::PromptBlock block;
        block.type = command::PromptBlockType::Text;
        block.text = "Base directory for this skill: " + base + "\n\n" + body;
        return std::vector<command::PromptBlock>{std::move(block)};
    };

    std::vector<std::shared_ptr<command::PromptCommand>> cmds;
    const auto make = [&](const std::string& name) {
        auto cmd = std::make_shared<command::PromptCommand>(name, fm.description);
        cmd->set_prompt_generator(gen);
        cmd->set_loaded_from(command::LoadSource::Skills);
        cmd->set_source("skills");
        cmd->set_user_invocable(fm.user_invocable);
        cmd->set_disable_model_invocation(fm.disable_model_invocation);
        if (fm.argument_hint) cmd->set_argument_hint(*fm.argument_hint);
        if (fm.context) cmd->set_context(*fm.context);
        if (fm.agent) cmd->set_agent(*fm.agent);
        if (!fm.hooks.empty()) cmd->set_hooks(fm.hooks);
        if (!fm.hooks_json.empty()) cmd->set_hooks_json(fm.hooks_json);
        if (!fm.paths.empty()) cmd->set_paths(fm.paths);
        return cmd;
    };

    cmds.push_back(make(fm.name));
    for (const auto& alias : fm.aliases) {
        cmds.push_back(make(alias));
    }
    return cmds;
}

}  // anonymous namespace

std::vector<std::shared_ptr<command::PromptCommand>> load_skills_from_dirs(
    const std::vector<std::string>& base_dirs) {
    std::vector<std::shared_ptr<command::PromptCommand>> result;
    std::unordered_map<std::string, bool> seen;  // canonical 路径去重
    std::unordered_set<std::string> seen_names;  // 命令名去重（近目录优先）

    std::error_code ec;
    for (const auto& base_dir : base_dirs) {
        fs::directory_iterator it(base_dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) continue;
        for (const auto& entry : it) {
            ec.clear();
            if (!entry.is_directory(ec) || ec) continue;

            const auto skill_dir = entry.path();
            const auto skill_file = skill_dir / "SKILL.md";
            const auto content = read_file_text(skill_file);
            if (!content) continue;

            const auto canonical = fs::weakly_canonical(skill_file, ec);
            if (ec) continue;
            const auto key = canonical.string();
            if (seen.contains(key)) continue;
            seen[key] = true;

            const auto parsed = parse_skill_content(*content, skill_dir.filename().string());
            auto cmds = build_commands(parsed, skill_dir);
            for (auto& cmd : cmds) {
                if (!seen_names.insert(cmd->name()).second) continue;
                result.push_back(std::move(cmd));
            }
        }
    }
    return result;
}

size_t register_bundled_skill(command::CommandRegistry& registry, const std::string& skill_dir,
                              std::unordered_set<std::string>* seen_names) {
    const auto content = read_file_text(fs::path(skill_dir) / "SKILL.md");
    if (!content) return 0;

    const auto parsed = parse_skill_content(*content, fs::path(skill_dir).filename().string());
    auto cmds = build_commands(parsed, fs::path(skill_dir));
    size_t n = 0;
    for (auto& cmd : cmds) {
        cmd->set_loaded_from(command::LoadSource::Bundled);
        if (seen_names && !seen_names->insert(cmd->name()).second)
            continue;  // #56 M-1：同名跳过（首个注册优先）
        registry.register_command(cmd);
        ++n;
    }
    return n;
}

std::string find_bundled_skills_dir() {
    const auto exe_dir = agent::process::ToolRegistry::executable_dir();
    if (exe_dir.empty()) return {};
    std::error_code ec;
    fs::path root = fs::path(exe_dir) / "skills" / "bundled";
    if (!fs::is_directory(root, ec) || ec) return {};
    return root.string();
}

size_t register_bundled_skills(command::CommandRegistry& registry, const std::string& root) {
    if (root.empty()) return 0;
    std::error_code ec;
    fs::directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    if (ec) return 0;

    size_t total = 0;
    std::unordered_set<std::string> seen_names;  // #56 M-1：跨技能命令名去重（首个注册优先）
    for (const auto& entry : it) {
        ec.clear();
        if (!entry.is_directory(ec) || ec) continue;
        total += register_bundled_skill(registry, entry.path().string(), &seen_names);
    }
    return total;
}

}  // namespace agent::skill
