/**
 * @file test_token_count.cpp
 * @brief Token 估算器单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/compact/token_count.h"

using namespace agent::compact;

TEST_CASE("rough_token_count basic", "[compact][token_count]") {
    SECTION("empty string returns 0") { REQUIRE(rough_token_count("") == 0); }

    SECTION("default bytes_per_token=4") {
        // 8 chars / 4 = 2 tokens
        REQUIRE(rough_token_count("12345678") == 2);
    }

    SECTION("rounds up") {
        // 7 chars / 4 = 1.75 -> ceil = 2
        REQUIRE(rough_token_count("1234567") == 2);
        // 5 chars / 4 = 1.25 -> ceil = 2
        REQUIRE(rough_token_count("12345") == 2);
        // 1 char / 4 = 0.25 -> ceil = 1
        REQUIRE(rough_token_count("a") == 1);
    }

    SECTION("custom bytes_per_token") {
        REQUIRE(rough_token_count("12345678", 2) == 4);
        REQUIRE(rough_token_count("12345678", 8) == 1);
    }

    SECTION("invalid bytes_per_token falls back to default") {
        // bytes_per_token=0 should not divide by zero
        REQUIRE(rough_token_count("abcd", 0) == 1);
        REQUIRE(rough_token_count("abcd", -1) == 1);
    }
}

TEST_CASE("bytes_per_token_for_ext", "[compact][token_count]") {
    SECTION("json variants return 2") {
        REQUIRE(bytes_per_token_for_ext("json") == 2);
        REQUIRE(bytes_per_token_for_ext("jsonl") == 2);
        REQUIRE(bytes_per_token_for_ext("jsonc") == 2);
    }

    SECTION("other extensions return 4") {
        REQUIRE(bytes_per_token_for_ext("cpp") == 4);
        REQUIRE(bytes_per_token_for_ext("py") == 4);
        REQUIRE(bytes_per_token_for_ext("md") == 4);
    }

    SECTION("empty extension returns default") { REQUIRE(bytes_per_token_for_ext("") == 4); }
}

TEST_CASE("estimate_message_tokens", "[compact][token_count]") {
    using namespace agent;

    SECTION("user message has content + overhead") {
        auto msg = ChatMessage::user("hello world");
        // "hello world" = 11 chars / 4 = 3 tokens (ceil)
        // + 4 overhead
        REQUIRE(estimate_message_tokens(msg) == 3 + 4);
    }

    SECTION("empty content still has overhead") {
        auto msg = ChatMessage::user("");
        REQUIRE(estimate_message_tokens(msg) == 4);
    }

    SECTION("assistant message with reasoning") {
        ChatMessage msg = ChatMessage::assistant("answer");
        msg.reasoning_content = "thinking about it";
        // "answer" = 6/4 = 2
        // "thinking about it" = 17/4 = 5 (ceil)
        // + 4 overhead
        REQUIRE(estimate_message_tokens(msg) == 2 + 5 + 4);
    }

    SECTION("tool_result message includes tool_call_id and tool_name") {
        auto msg = ChatMessage::tool_result("toolu_123", "Read", "file content here");
        // tool_call_id "toolu_123" = 9/4 = 3 (ceil)
        // tool_name "Read" = 4/4 = 1
        // content "file content here" = 17/4 = 5 (ceil)
        // + 4 overhead
        REQUIRE(estimate_message_tokens(msg) == 3 + 1 + 5 + 4);
    }

    SECTION("assistant message with tool_uses") {
        ChatMessage msg = ChatMessage::assistant("");
        ToolUse tu;
        tu.id = "toolu_abc";
        tu.name = "Write";
        tu.input = nlohmann::json::object({{"path", "/tmp/test.txt"}, {"content", "hello"}});
        msg.tool_uses.push_back(tu);
        // tu.id "toolu_abc" = 9/4 = 3
        // tu.name "Write" = 5/4 = 2
        // tu.input serialized (JSON ratio=2)
        int32_t expected = estimate_message_tokens(msg);
        REQUIRE(expected > 4);  // at least overhead + something
    }
}

TEST_CASE("estimate_messages_tokens accumulates", "[compact][token_count]") {
    using namespace agent;

    SECTION("single message equals estimate_message_tokens") {
        std::vector<ChatMessage> msgs = {ChatMessage::user("hello")};
        int32_t single = estimate_message_tokens(msgs[0]);
        REQUIRE(estimate_messages_tokens(msgs) == single);
    }

    SECTION("multiple messages accumulate") {
        std::vector<ChatMessage> msgs = {ChatMessage::user("hello"),
                                         ChatMessage::assistant("hi there"),
                                         ChatMessage::user("bye")};
        int32_t sum = estimate_message_tokens(msgs[0]) + estimate_message_tokens(msgs[1]) +
                      estimate_message_tokens(msgs[2]);
        REQUIRE(estimate_messages_tokens(msgs) == sum);
    }

    SECTION("empty vector returns 0") {
        std::vector<ChatMessage> msgs;
        REQUIRE(estimate_messages_tokens(msgs) == 0);
    }
}
