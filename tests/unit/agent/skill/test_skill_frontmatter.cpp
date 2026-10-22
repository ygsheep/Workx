/**
 * @file test_skill_frontmatter.cpp
 * @brief Skill frontmatter 解析器单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/skill/inclaude/frontmatter.h"

using namespace agent::skill;

TEST_CASE("parse full frontmatter", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "name: my-skill\n"
        "description: Does the thing\n"
        "aliases: ms, my\n"
        "argument_hint: [file]\n"
        "when_to_use: When doing the thing\n"
        "model: claude-sonnet\n"
        "user_invocable: true\n"
        "disable_model_invocation: false\n"
        "---\n"
        "# My Skill\n"
        "Body text here\n";

    const auto parsed = parse_skill_content(content, "dir-name");

    REQUIRE(parsed.frontmatter.name == "my-skill");
    REQUIRE(parsed.frontmatter.description == "Does the thing");
    REQUIRE(parsed.frontmatter.aliases.size() == 2);
    REQUIRE(parsed.frontmatter.aliases[0] == "ms");
    REQUIRE(parsed.frontmatter.aliases[1] == "my");
    REQUIRE(parsed.frontmatter.argument_hint.has_value());
    REQUIRE(parsed.frontmatter.argument_hint.value() == "[file]");
    REQUIRE(parsed.frontmatter.when_to_use.has_value());
    REQUIRE(parsed.frontmatter.when_to_use.value() == "When doing the thing");
    REQUIRE(parsed.frontmatter.model.has_value());
    REQUIRE(parsed.frontmatter.model.value() == "claude-sonnet");
    REQUIRE(parsed.frontmatter.user_invocable);
    REQUIRE_FALSE(parsed.frontmatter.disable_model_invocation);

    REQUIRE(parsed.body == "# My Skill\nBody text here\n");
}

TEST_CASE("name defaults to directory name", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "description: no name field\n"
        "---\n"
        "body\n";

    const auto parsed = parse_skill_content(content, "dir-name");
    REQUIRE(parsed.frontmatter.name == "dir-name");
}

TEST_CASE("no frontmatter: whole content is body, description derived from first line",
          "[skill][frontmatter]") {
    const std::string content = "# Plain Skill\nsome body\n";

    const auto parsed = parse_skill_content(content, "dir-name");

    REQUIRE(parsed.frontmatter.name == "dir-name");
    REQUIRE(parsed.frontmatter.description == "Plain Skill");
    REQUIRE(parsed.body == "# Plain Skill\nsome body\n");
}

TEST_CASE("unclosed frontmatter is treated as body", "[skill][frontmatter]") {
    const std::string content = "---\nname: broken\njust text\n";

    const auto parsed = parse_skill_content(content, "dir-name");

    REQUIRE(parsed.frontmatter.name == "dir-name");
    REQUIRE(parsed.body == content);
}

TEST_CASE("empty content", "[skill][frontmatter]") {
    const auto parsed = parse_skill_content("", "dir-name");
    REQUIRE(parsed.frontmatter.name == "dir-name");
    REQUIRE(parsed.frontmatter.description.empty());
    REQUIRE(parsed.body.empty());
}

TEST_CASE("aliases bracket form and whitespace", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "aliases: [foo, bar , baz]\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.aliases.size() == 3);
    REQUIRE(parsed.frontmatter.aliases[0] == "foo");
    REQUIRE(parsed.frontmatter.aliases[1] == "bar");
    REQUIRE(parsed.frontmatter.aliases[2] == "baz");
}

TEST_CASE("aliases strip yaml quotes in bracket and multiline forms", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "aliases: ['f1', \"f2\"]\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.aliases == std::vector<std::string>{"f1", "f2"});
}

TEST_CASE("multiline aliases strip yaml quotes", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "aliases:\n"
        "- \"f3\"\n"
        "- f4\n"
        "paths:\n"
        "- 'src/**/*.ts'\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.aliases == std::vector<std::string>{"f3", "f4"});
    REQUIRE(parsed.frontmatter.paths == std::vector<std::string>{"src/**/*.ts"});
}

TEST_CASE("context agent and hooks fields parse", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "context: 处理 React 组件时\n"
        "agent: frontend\n"
        "hooks: echo a, echo b\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.context.value() == "处理 React 组件时");
    REQUIRE(parsed.frontmatter.agent.value() == "frontend");
    REQUIRE(parsed.frontmatter.hooks == std::vector<std::string>{"echo a", "echo b"});
}

TEST_CASE("multiline hooks strip quotes", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "hooks:\n"
        "- \"echo one\"\n"
        "- 'echo two'\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.hooks == std::vector<std::string>{"echo one", "echo two"});
}

TEST_CASE("context agent hooks absent by default", "[skill][frontmatter]") {
    const std::string content = "---\nname: plain\n---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE_FALSE(parsed.frontmatter.context.has_value());
    REQUIRE_FALSE(parsed.frontmatter.agent.has_value());
    REQUIRE(parsed.frontmatter.hooks.empty());
}

TEST_CASE("bool parsing variants", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "user_invocable: no\n"
        "disable_model_invocation: yes\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE_FALSE(parsed.frontmatter.user_invocable);
    REQUIRE(parsed.frontmatter.disable_model_invocation);
}

TEST_CASE("invalid bool falls back to default", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "user_invocable: maybe\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.user_invocable);
}

TEST_CASE("malformed and unknown lines are ignored", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "no colon here\n"
        "# comment line\n"
        "unknown_field: ignored\n"
        "description: kept\n"
        "---\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.description == "kept");
    REQUIRE_FALSE(parsed.frontmatter.when_to_use.has_value());
}

TEST_CASE("explicit empty description is derived from body", "[skill][frontmatter]") {
    const std::string content =
        "---\n"
        "description:\n"
        "---\n"
        "# Derived Title\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.description == "Derived Title");
}

TEST_CASE("CRLF line endings", "[skill][frontmatter]") {
    const std::string content =
        "---\r\n"
        "name: win-skill\r\n"
        "description: CRLF file\r\n"
        "---\r\n"
        "# Body\r\n";

    const auto parsed = parse_skill_content(content, "dir");
    REQUIRE(parsed.frontmatter.name == "win-skill");
    REQUIRE(parsed.frontmatter.description == "CRLF file");
    REQUIRE(parsed.body == "# Body\r\n");
}
