/**
 * @file test_conditional.cpp
 * @brief Conditional Skills 单元测试：touch 收集、glob 匹配、激活
 */

#include <catch2/catch_test_macros.hpp>

#include <thread>
#include <vector>

#include "agent/skill/inclaude/conditional.h"
#include "agent/skill/inclaude/frontmatter.h"
#include "agent/skill/inclaude/skill_loader.h"
#include "agent/tool/path_matcher.h"

using namespace agent::skill;
using namespace agent::command;

namespace {

std::shared_ptr<PromptCommand> make_skill_with_paths(const std::string& name,
                                                     const std::vector<std::string>& paths) {
    auto cmd = std::make_shared<PromptCommand>(name, "desc");
    cmd->set_paths(paths);
    cmd->set_prompt_generator([name](const std::string&, const CommandContext&) {
        PromptBlock block;
        block.type = PromptBlockType::Text;
        block.text = name + " body";
        return std::vector<PromptBlock>{std::move(block)};
    });
    return cmd;
}

}  // anonymous namespace

TEST_CASE("frontmatter parses paths as array and comma list", "[skill][conditional]") {
    const auto parsed = parse_skill_content(
        "---\n"
        "name: frontend\n"
        "paths:\n"
        "  - \"src/**/*.tsx\"\n"
        "  - 'src/**/*.css'\n"
        "---\n"
        "body\n",
        "frontend");
    REQUIRE(parsed.frontmatter.paths.size() == 2);
    REQUIRE(parsed.frontmatter.paths[0] == "src/**/*.tsx");
    REQUIRE(parsed.frontmatter.paths[1] == "src/**/*.css");
}

TEST_CASE("frontmatter parses comma-separated paths", "[skill][conditional]") {
    const auto parsed = parse_skill_content(
        "---\n"
        "name: tool\n"
        "paths: src/**/*.cpp, src/**/*.h\n"
        "---\n"
        "body\n",
        "tool");
    REQUIRE(parsed.frontmatter.paths.size() == 2);
    REQUIRE(parsed.frontmatter.paths[0] == "src/**/*.cpp");
    REQUIRE(parsed.frontmatter.paths[1] == "src/**/*.h");
}

TEST_CASE("frontmatter without paths leaves empty", "[skill][conditional]") {
    const auto parsed = parse_skill_content(
        "---\n"
        "name: plain\n"
        "description: d\n"
        "---\n"
        "body\n",
        "plain");
    REQUIRE(parsed.frontmatter.paths.empty());
}

TEST_CASE("TouchCollector deduplicates and normalizes", "[skill][conditional]") {
    TouchCollector collector;
    collector.add("c:/proj/src/a.cpp");
    collector.add("c:/proj/src/a.cpp");
    collector.add("");
    collector.add("c:/proj/src/b.cpp");

    const auto paths = collector.paths();
    REQUIRE(paths.size() == 2);

    collector.clear();
    REQUIRE(collector.paths().empty());
}

TEST_CASE("TouchCollector is thread-safe", "[skill][conditional]") {
    TouchCollector collector;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&collector, t]() {
            for (int i = 0; i < 100; ++i) {
                collector.add("c:/proj/src/file" + std::to_string(t) + "_" + std::to_string(i) +
                              ".cpp");
            }
        });
    }
    for (auto& th : threads) th.join();
    REQUIRE(collector.paths().size() == 800);
}

TEST_CASE("skill_matches_touch: relative pattern vs relative path", "[skill][conditional]") {
    const auto skill = make_skill_with_paths("fe", {"src/**/*.tsx"});
    REQUIRE(skill_matches_touch("c:/proj/src/components/Button.tsx", *skill, "c:/proj"));
    REQUIRE_FALSE(skill_matches_touch("c:/proj/tests/Button.test.tsx", *skill, "c:/proj"));
}

TEST_CASE("skill_matches_touch: absolute pattern", "[skill][conditional]") {
    // 相对 pattern 匹配 cwd 下深层路径
    const auto skill = make_skill_with_paths("logs", {"data/**/*.log"});
    REQUIRE(skill_matches_touch("c:/proj/data/app.log", *skill, "c:/proj"));
    REQUIRE_FALSE(skill_matches_touch("c:/proj/tmp/app.log", *skill, "c:/proj"));
    // cwd 外路径不命中相对 pattern
    REQUIRE_FALSE(skill_matches_touch("c:/elsewhere/data/app.log", *skill, "c:/proj"));
}

TEST_CASE("skill_matches_touch: windows drive absolute pattern", "[skill][conditional]") {
    // 盘符绝对 pattern（POSIX 斜杠）：不再被误判为相对 pattern
    const auto skill = make_skill_with_paths("wfe", {"c:/proj/src/**/*.ts"});
    REQUIRE(skill_matches_touch("c:/proj/src/components/a.ts", *skill, "c:/proj"));
    REQUIRE_FALSE(skill_matches_touch("c:/proj/tests/a.ts", *skill, "c:/proj"));
    // 盘符绝对 pattern（反斜杠形式）：to_posix_path 归一后同样命中
    // 盘符大小写必须与被测路径一致：to_posix_path 只做 '\' → '/' 替换，不做大小写归一；
    // 匹配在 Windows 上大小写不敏感、在 Linux 上敏感，写成 "C:\\" 配 "c:/" 只在
    // Windows 上碰巧通过（#109）。用例本意是验证反斜杠归一，不是大小写，
    // 故统一用小写盘符，让断言在两个平台上都成立。
    const auto skill2 = make_skill_with_paths("wfe2", {"c:\\proj\\src\\*.ts"});
    REQUIRE(skill_matches_touch("c:/proj/src/a.ts", *skill2, "c:/proj"));
}

TEST_CASE("skill with empty paths never matches", "[skill][conditional]") {
    const auto skill = make_skill_with_paths("plain", {});
    REQUIRE_FALSE(skill_matches_touch("c:/proj/src/a.cpp", *skill, "c:/proj"));
}

TEST_CASE("activate_conditional_skills returns matched only", "[skill][conditional]") {
    std::vector<std::shared_ptr<CommandBase>> skills{
        make_skill_with_paths("fe", {"src/**/*.tsx"}),
        make_skill_with_paths("cpp", {"src/**/*.cpp"}),
        make_skill_with_paths("plain", {}),
    };
    const std::vector<std::string> touched{
        "c:/proj/src/a.cpp",
        "c:/proj/tests/x.tsx",  // tests 不在 src 下，不命中 fe
    };

    const auto activated = activate_conditional_skills(touched, skills, "c:/proj");

    REQUIRE(activated.size() == 1);
    REQUIRE(activated[0]->name() == "cpp");
}

TEST_CASE("activate_conditional_skills ignores non-prompt commands", "[skill][conditional]") {
    std::vector<std::shared_ptr<CommandBase>> skills{
        make_skill_with_paths("fe", {"src/**/*.tsx"}),
        std::make_shared<LocalCommand>("local", "d"),
    };
    const std::vector<std::string> touched{"c:/proj/src/a.tsx"};

    const auto activated = activate_conditional_skills(touched, skills, "c:/proj");

    REQUIRE(activated.size() == 1);
    REQUIRE(activated[0]->name() == "fe");
}

TEST_CASE("loader attaches paths from frontmatter", "[skill][conditional]") {
    const auto parsed = parse_skill_content(
        "---\n"
        "name: cond\n"
        "paths: src/**/*.ts\n"
        "---\n"
        "body\n",
        "cond");
    auto cmd =
        std::make_shared<PromptCommand>(parsed.frontmatter.name, parsed.frontmatter.description);
    if (!parsed.frontmatter.paths.empty()) cmd->set_paths(parsed.frontmatter.paths);

    REQUIRE(cmd->paths().size() == 1);
    REQUIRE(cmd->paths()[0] == "src/**/*.ts");
}
