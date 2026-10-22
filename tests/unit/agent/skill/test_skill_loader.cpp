/**
 * @file test_skill_loader.cpp
 * @brief Skill 磁盘加载器单元测试
 * @details 使用临时目录构造 .claude/skills 结构，验证扫描/解析/去重/别名
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include "agent/skill/inclaude/skill_loader.h"

namespace fs = std::filesystem;
using namespace agent::skill;
using namespace agent::command;

namespace {

/// @brief 临时目录 RAII（同工具测试惯例）
class TempDir {
   public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(100000, 999999);
        path_ = fs::temp_directory_path() / ("workx_skill_test_" + std::to_string(dist(gen)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    fs::path make_file(const fs::path& rel, const std::string& content) const {
        auto fp = path_ / rel;
        fs::create_directories(fp.parent_path());
        std::ofstream out(fp, std::ios::binary);
        out << content;
        out.close();
        return fp;
    }
    const fs::path& path() const { return path_; }

   private:
    fs::path path_;
};

const std::string kSample =
    "---\n"
    "name: sample\n"
    "description: A sample skill\n"
    "aliases: s, smpl\n"
    "---\n"
    "# Sample\n"
    "Do the thing\n";

}  // anonymous namespace

TEST_CASE("load a valid skill from dir", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 3);  // 本体 + 2 个别名
    REQUIRE(cmds[0]->name() == "sample");
    REQUIRE(cmds[0]->description() == "A sample skill");
    REQUIRE(cmds[0]->loaded_from() == LoadSource::Skills);
    REQUIRE(cmds[0]->source() == "skills");
    REQUIRE(cmds[0]->is_user_invocable());
    REQUIRE_FALSE(cmds[0]->is_model_invocation_disabled());
    REQUIRE(cmds[0]->type() == "prompt");
}

TEST_CASE("loader attaches context agent hooks", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/ctx/SKILL.md",
                  "---\n"
                  "name: ctx\n"
                  "context: React 场景\n"
                  "agent: frontend\n"
                  "hooks:\n"
                  "- echo one\n"
                  "---\n"
                  "body\n");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0]->context().value() == "React 场景");
    REQUIRE(cmds[0]->agent().value() == "frontend");
    REQUIRE(cmds[0]->hooks() == std::vector<std::string>{"echo one"});
}

TEST_CASE("aliases become separate commands sharing the prompt", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 3);
    REQUIRE(cmds[1]->name() == "s");
    REQUIRE(cmds[2]->name() == "smpl");
    REQUIRE(cmds[1]->loaded_from() == LoadSource::Skills);
}

TEST_CASE("generator emits baseDir prefix plus body", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});
    REQUIRE(cmds.size() == 3);

    const auto blocks = cmds[0]->generate_prompt("", CommandContext{});
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].type == PromptBlockType::Text);
    REQUIRE(blocks[0].text.find("Base directory for this skill:") != std::string::npos);
    REQUIRE(blocks[0].text.find("# Sample") != std::string::npos);
}

TEST_CASE("dirs without SKILL.md are skipped", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    tmp.make_file("proj/.claude/skills/no-skill/notes.txt", "not a skill");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 3);
    for (const auto& cmd : cmds) {
        REQUIRE(cmd->name() != "no-skill");
    }
}

TEST_CASE("name falls back to directory name", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/fallback/SKILL.md",
                  "---\n"
                  "description: no name field\n"
                  "---\n"
                  "body\n");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0]->name() == "fallback");
}

TEST_CASE("user_invocable false is honored", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/private/SKILL.md",
                  "---\n"
                  "user_invocable: false\n"
                  "---\n");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 1);
    REQUIRE_FALSE(cmds[0]->is_user_invocable());
}

TEST_CASE("same file listed from two dirs is deduplicated", "[skill][loader]") {
    TempDir tmp;
    const auto file = tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir_a = (tmp.path() / "proj/.claude/skills").string();
    // 通过符号链接制造同一文件的第二个入口（Windows 需管理员权限时用复制路径模拟）
    std::error_code ec;
    fs::create_directory_symlink(file.parent_path(), tmp.path() / "alias_dir", ec);
    std::vector<std::string> dirs{dir_a};
    if (!ec) {
        dirs.push_back((tmp.path() / "alias_dir").string());
    }

    const auto cmds = load_skills_from_dirs(dirs);

    REQUIRE(cmds.size() == 3);
}

TEST_CASE("find_skill_dirs_up_to_home returns deepest first", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("a/.claude/skills/one/SKILL.md", kSample);
    tmp.make_file("a/b/.claude/skills/two/SKILL.md", kSample);
    const auto cwd = (tmp.path() / "a" / "b").string();

    const auto dirs = find_skill_dirs_up_to_home(cwd);

    REQUIRE(dirs.size() >= 2);
    REQUIRE(dirs[0] == (fs::path(cwd) / ".claude" / "skills").string());
    REQUIRE(dirs[1] == (tmp.path() / "a" / ".claude" / "skills").string());
}

TEST_CASE("same name in two dirs keeps the first (nearer wins)", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("near/.claude/skills/dup/SKILL.md",
                  "---\n"
                  "name: dup\n"
                  "description: near version\n"
                  "---\n"
                  "near body\n");
    tmp.make_file("far/.claude/skills/dup/SKILL.md",
                  "---\n"
                  "name: dup\n"
                  "description: far version\n"
                  "---\n"
                  "far body\n");
    const auto near_dir = (tmp.path() / "near/.claude/skills").string();
    const auto far_dir = (tmp.path() / "far/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({near_dir, far_dir});

    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0]->description() == "near version");
}

TEST_CASE("alias conflict with another skill's name resolves to first", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("near/.claude/skills/dup/SKILL.md",
                  "---\n"
                  "name: dup\n"
                  "description: near version\n"
                  "aliases: shared\n"
                  "---\n"
                  "near body\n");
    tmp.make_file("far/.claude/skills/other/SKILL.md",
                  "---\n"
                  "name: other\n"
                  "description: far version\n"
                  "aliases: shared\n"
                  "---\n"
                  "far body\n");
    const auto near_dir = (tmp.path() / "near/.claude/skills").string();
    const auto far_dir = (tmp.path() / "far/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({near_dir, far_dir});

    // near: dup + shared；far: other（shared 冲突丢弃）
    REQUIRE(cmds.size() == 3);
}

TEST_CASE("register_bundled_skill registers with Bundled source", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("bundled/alpha/SKILL.md",
                  "---\n"
                  "name: alpha\n"
                  "description: built-in skill\n"
                  "aliases: a\n"
                  "---\n"
                  "# Alpha\n"
                  "Do alpha things\n");

    CommandRegistry registry;
    const auto count =
        register_bundled_skill(registry, (tmp.path() / "bundled" / "alpha").string());

    REQUIRE(count == 2);
    const auto cmd = registry.find_by_name("alpha");
    REQUIRE(cmd != nullptr);
    REQUIRE(cmd->type() == "prompt");
    REQUIRE(cmd->loaded_from() == LoadSource::Bundled);
    REQUIRE(registry.find_by_name("a") != nullptr);
    REQUIRE(registry.find_by_name("a")->loaded_from() == LoadSource::Bundled);

    const auto* prompt_cmd = dynamic_cast<const PromptCommand*>(cmd.get());
    REQUIRE(prompt_cmd != nullptr);
    const auto blocks = prompt_cmd->generate_prompt("", CommandContext{});
    REQUIRE_FALSE(blocks.empty());
    REQUIRE(blocks[0].text.find("Base directory for this skill:") != std::string::npos);
}

TEST_CASE("register_bundled_skill skips missing SKILL.md", "[skill][loader]") {
    TempDir tmp;
    fs::create_directories(tmp.path() / "bundled/empty");

    CommandRegistry registry;
    const auto count =
        register_bundled_skill(registry, (tmp.path() / "bundled" / "empty").string());

    REQUIRE(count == 0);
    REQUIRE(registry.size() == 0);
}

TEST_CASE("find_user_skill_dirs returns existing home dirs", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file(".claude/skills/u1/SKILL.md", kSample);
    tmp.make_file(".workx/skills/u2/SKILL.md", kSample);

#if defined(_WIN32)
    const char* env_var = "USERPROFILE";
#else
    const char* env_var = "HOME";
#endif
    const char* saved = std::getenv(env_var);
    std::string saved_str = saved ? saved : "";
    const auto set_env = [&](const std::string& v) {
#if defined(_WIN32)
        _putenv_s(env_var, v.c_str());
#else
        setenv(env_var, v.c_str(), 1);
#endif
    };

    set_env(tmp.path().string());
    const auto dirs = find_user_skill_dirs();
    if (saved) {
        set_env(saved_str);
    }
#if defined(_WIN32)
    else {
        _putenv_s(env_var, "");
    }
#else
    else {
        unsetenv(env_var);
    }
#endif

    REQUIRE(dirs.size() == 2);
    REQUIRE(dirs[0] == (tmp.path() / ".claude" / "skills").string());
    REQUIRE(dirs[1] == (tmp.path() / ".workx" / "skills").string());
}

TEST_CASE("register_bundled_skills registers all subdirs with Bundled source", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("bundled/loop/SKILL.md",
                  "---\nname: loop\ndescription: iterate\n---\n# Loop\nDo loop\n");
    tmp.make_file("bundled/debug/SKILL.md",
                  "---\nname: debug\ndescription: debug things\n---\n# Debug\nDo debug\n");
    // 缺 SKILL.md 的子目录应被跳过
    fs::create_directories(tmp.path() / "bundled/empty");

    CommandRegistry registry;
    const auto count = register_bundled_skills(registry, (tmp.path() / "bundled").string());

    REQUIRE(count == 2);
    REQUIRE(registry.size() == 2);
    REQUIRE(registry.find_by_name("loop")->loaded_from() == LoadSource::Bundled);
    REQUIRE(registry.find_by_name("debug")->loaded_from() == LoadSource::Bundled);
    REQUIRE(registry.find_by_name("empty") == nullptr);
}

TEST_CASE("register_bundled_skills handles empty or missing root", "[skill][loader]") {
    TempDir tmp;

    CommandRegistry empty_reg;
    REQUIRE(register_bundled_skills(empty_reg, "") == 0);

    CommandRegistry missing_reg;
    REQUIRE(register_bundled_skills(missing_reg, (tmp.path() / "no_such_dir").string()) == 0);

    CommandRegistry empty_root_reg;
    fs::create_directories(tmp.path() / "empty_root");
    REQUIRE(register_bundled_skills(empty_root_reg, (tmp.path() / "empty_root").string()) == 0);
}
