/**
 * @file conditional.cpp
 * @brief Conditional Skills 实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/skill/inclaude/conditional.h"

#include <filesystem>

#include "agent/skill/inclaude/skill_loader.h"
#include "agent/tool/path_matcher.h"

namespace fs = std::filesystem;

namespace agent::skill {

void TouchCollector::add(const std::string& path) {
    if (path.empty()) return;
    std::string normalized = path;
    std::error_code ec;
    const auto abs = fs::weakly_canonical(fs::absolute(path, ec), ec);
    if (!ec && !abs.empty()) {
        normalized = abs.string();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paths.insert(std::move(normalized));
}

std::vector<std::string> TouchCollector::paths() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return {m_paths.begin(), m_paths.end()};
}

void TouchCollector::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_paths.clear();
}

namespace {

/// @brief 路径相对 cwd 化（POSIX）：返回相对路径；不在 cwd 下则原样返回绝对路径
std::string relativize(std::string_view path, std::string_view cwd) {
    const std::string posix_cwd = agent::tool::to_posix_path(cwd);
    std::string posix_path = agent::tool::to_posix_path(path);
    if (!posix_cwd.empty() && posix_path.size() > posix_cwd.size() &&
        posix_path.compare(0, posix_cwd.size(), posix_cwd) == 0 &&
        (posix_cwd.back() == '/' || posix_path[posix_cwd.size()] == '/')) {
        size_t skip = posix_cwd.back() == '/' ? posix_cwd.size() : posix_cwd.size() + 1;
        if (skip < posix_path.size()) {
            return posix_path.substr(skip);
        }
    }
    return posix_path;
}

}  // anonymous namespace

bool skill_matches_touch(const std::string& path, const command::CommandBase& skill,
                         const std::string& cwd) {
    const auto& patterns = skill.paths();
    if (patterns.empty()) return false;

    const std::string posix_path = agent::tool::to_posix_path(path);
    for (const auto& raw_pattern : patterns) {
        auto pattern = agent::tool::to_posix_path(agent::tool::expand_home(raw_pattern));
        // 绝对 pattern：POSIX '/' 开头，或 Windows 盘符（X:/）
        const bool is_absolute =
            !pattern.empty() &&
            (pattern[0] == '/' ||
             (pattern.size() >= 2 && std::isalpha(static_cast<unsigned char>(pattern[0])) &&
              pattern[1] == ':'));
        // 相对 pattern：touch 路径相对 cwd 后匹配；绝对 pattern：直接匹配
        const std::string& candidate = is_absolute ? posix_path : relativize(posix_path, cwd);
        if (agent::tool::match_path_glob(candidate, pattern)) {
            return true;
        }
    }
    return false;
}

std::vector<std::shared_ptr<command::PromptCommand>> activate_conditional_skills(
    const std::vector<std::string>& touched,
    const std::vector<std::shared_ptr<command::CommandBase>>& skills, const std::string& cwd) {
    std::vector<std::shared_ptr<command::PromptCommand>> result;
    for (const auto& skill : skills) {
        auto* prompt_cmd = dynamic_cast<command::PromptCommand*>(skill.get());
        if (!prompt_cmd || prompt_cmd->paths().empty()) continue;
        bool matched = false;
        for (const auto& path : touched) {
            if (skill_matches_touch(path, *prompt_cmd, cwd)) {
                matched = true;
                break;
            }
        }
        if (matched) result.push_back(std::dynamic_pointer_cast<command::PromptCommand>(skill));
    }
    return result;
}

}  // namespace agent::skill
