/**
 * @file test_memory.cpp
 * @brief 项目记忆加载单元测试
 * @details #86：候选文件名由「CLAUDE.md / AGENT.md 二选一」扩展为
 *          「CLAUDE.md > AGENTS.md > AGENT.md 按顺序取第一个存在的」。
 *          AGENTS.md（带 S）此前完全不被识别，只有它的项目其规范会静默不生效。
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "agent/prompt/memory.h"

namespace fs = std::filesystem;
using agent::prompt::MemoryFileInfo;

namespace {

/// @brief 临时目录 RAII：构造时清空重建，析构时删除
class TempDir {
   public:
    explicit TempDir(const std::string& name)
        : path_(fs::temp_directory_path() / ("workx_test_memory_" + name)) {
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

    /// @brief 在临时目录下写文件（自动创建父目录）
    void write(const std::string& rel, const std::string& content) const {
        const fs::path p = path_ / rel;
        std::error_code ec;
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path(), ec);
        }
        std::ofstream ofs(p, std::ios::binary);
        ofs << content;
    }

   private:
    fs::path path_;
};

/// @brief p 是否位于 base 之内（含 base 自身）
bool is_under(const fs::path& p, const fs::path& base) {
    const std::string b = fs::weakly_canonical(base).string();
    const std::string s = fs::weakly_canonical(p).string();
    if (s.size() < b.size() || s.compare(0, b.size(), b) != 0) return false;
    if (s.size() == b.size()) return true;
    return s[b.size()] == fs::path::preferred_separator;
}

/// @brief 加载并只保留 base 之内的记忆文件
/// @details load_project_memory 会从 CWD 一路向上遍历到文件系统根，祖先目录若恰好
///          存在同名文件会污染结果，故统一过滤，保证用例与环境无关。
std::vector<MemoryFileInfo> filtered(const fs::path& base, const fs::path& cwd) {
    std::vector<MemoryFileInfo> out;
    for (const auto& f : agent::prompt::load_project_memory(cwd)) {
        if (is_under(f.path, base)) out.push_back(f);
    }
    return out;
}

std::vector<std::string> names(const std::vector<MemoryFileInfo>& files) {
    std::vector<std::string> out;
    for (const auto& f : files) out.push_back(f.path.filename().string());
    return out;
}

}  // namespace

TEST_CASE("#86 只有 AGENTS.md 的目录能被加载", "[memory][86]") {
    // 修复前：AGENTS.md 从未被尝试，该项目规范静默不生效且无任何提示
    TempDir dir("agents_only");
    dir.write("AGENTS.md", "convention-from-AGENTS");

    const auto files = filtered(dir.path(), dir.path());
    REQUIRE(names(files) == std::vector<std::string>{"AGENTS.md"});
    REQUIRE(files.front().content == "convention-from-AGENTS");
}

TEST_CASE("#86 只有 CLAUDE.md 时命中 CLAUDE.md", "[memory][86]") {
    TempDir dir("claude_only");
    dir.write("CLAUDE.md", "convention-from-CLAUDE");

    const auto files = filtered(dir.path(), dir.path());
    REQUIRE(names(files) == std::vector<std::string>{"CLAUDE.md"});
}

TEST_CASE("#86 只有 AGENT.md 时命中 AGENT.md", "[memory][86]") {
    // 无 S 的旧约定仍需继续支持
    TempDir dir("agent_only");
    dir.write("AGENT.md", "convention-from-AGENT");

    const auto files = filtered(dir.path(), dir.path());
    REQUIRE(names(files) == std::vector<std::string>{"AGENT.md"});
}

TEST_CASE("#86 三个候选同时存在时取 CLAUDE.md", "[memory][86]") {
    // 优先级：CLAUDE.md > AGENTS.md > AGENT.md。本仓库根目录即此形态，
    // 修复不应改变既有行为（仍加载 CLAUDE.md）。
    TempDir dir("all_three");
    dir.write("CLAUDE.md", "from-claude");
    dir.write("AGENTS.md", "from-agents");
    dir.write("AGENT.md", "from-agent");

    const auto files = filtered(dir.path(), dir.path());
    REQUIRE(names(files) == std::vector<std::string>{"CLAUDE.md"});
    REQUIRE(files.front().content == "from-claude");
}

TEST_CASE("#86 AGENTS.md 与 AGENT.md 同时存在时取 AGENTS.md", "[memory][86]") {
    TempDir dir("agents_and_agent");
    dir.write("AGENT.md", "from-agent");
    dir.write("AGENTS.md", "from-agents");

    const auto files = filtered(dir.path(), dir.path());
    REQUIRE(names(files) == std::vector<std::string>{"AGENTS.md"});
    REQUIRE(files.front().content == "from-agents");
}

TEST_CASE("#86 无候选文件时不报错且返回空", "[memory][86]") {
    TempDir dir("no_memory_file");
    dir.write("README.md", "not a memory file");

    REQUIRE(filtered(dir.path(), dir.path()).empty());

    // 目录不存在时同样静默跳过，不抛异常
    REQUIRE_NOTHROW(agent::prompt::load_and_format_project_memory(dir.path() / "missing_subdir"));
}

TEST_CASE("#86 多级目录每级各取一个，顺序从根到 CWD", "[memory][86]") {
    // 父级用 CLAUDE.md、子级只有 AGENTS.md，两级都应命中且父级在前
    TempDir dir("multi_level");
    dir.write("CLAUDE.md", "parent-claude");
    dir.write("child/AGENTS.md", "child-agents");

    const fs::path cwd = dir.path() / "child";
    const auto files = filtered(dir.path(), cwd);
    REQUIRE(names(files) == std::vector<std::string>{"CLAUDE.md", "AGENTS.md"});
    REQUIRE(files.back().content == "child-agents");
}

TEST_CASE("#86 空记忆文件仍算命中", "[memory][86]") {
    // 存在即命中，不能因为内容为空就去尝试下一个候选（否则语义变成「取第一个非空」）
    TempDir dir("empty_claude");
    dir.write("CLAUDE.md", "");
    dir.write("AGENTS.md", "from-agents");

    const auto files = filtered(dir.path(), dir.path());
    REQUIRE(names(files) == std::vector<std::string>{"CLAUDE.md"});
    REQUIRE(files.front().content.empty());
}
