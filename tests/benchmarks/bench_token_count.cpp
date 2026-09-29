/**
 * @file bench_token_count.cpp
 * @brief Token 估算性能基准（Q-2）
 * @details 测量 compact::rough_token_count 和 estimate_messages_tokens 吞吐
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include "agent/compact/token_count.h"
#include "agent/api/chat_types.h"

#include <string>
#include <vector>

using namespace agent;
using namespace agent::compact;

TEST_CASE("rough_token_count throughput", "[benchmark][token_count]") {
    // 100KB 文本
    std::string text(100'000, 'x');

    BENCHMARK("rough_token_count 100KB text") { return rough_token_count(text); };

    // 含 JSON 内容
    std::string json_text = R"({"key":"value","nested":{"arr":[1,2,3],"num":42}})";
    // 重复 1000 次构造 100KB+ JSON
    std::string large_json;
    large_json.reserve(json_text.size() * 1000);
    for (int i = 0; i < 1000; ++i) large_json += json_text;

    BENCHMARK("rough_token_count 100KB JSON") {
        return rough_token_count(large_json, BYTES_PER_TOKEN_JSON);
    };
}

TEST_CASE("estimate_messages_tokens throughput", "[benchmark][token_count]") {
    // 构造 100 条消息，每条 1KB 内容
    std::vector<ChatMessage> messages;
    messages.reserve(100);
    for (int i = 0; i < 100; ++i) {
        ChatMessage msg;
        msg.role = ChatMessage::Role::User;
        msg.content = std::string(1000, 'x');
        messages.push_back(std::move(msg));
    }

    BENCHMARK("estimate_messages_tokens 100 messages x 1KB") {
        return estimate_messages_tokens(messages);
    };

    // 模拟真实对话：含 tool_use 块
    std::vector<ChatMessage> mixed_messages;
    for (int i = 0; i < 50; ++i) {
        ChatMessage user_msg;
        user_msg.role = ChatMessage::Role::User;
        user_msg.content = "请帮我读取 src/main.cpp 文件";
        mixed_messages.push_back(std::move(user_msg));

        ChatMessage asst_msg;
        asst_msg.role = ChatMessage::Role::Assistant;
        asst_msg.content = "我来帮你读取文件内容。";
        ToolUse tu;
        tu.id = "toolu_" + std::to_string(i);
        tu.name = "Read";
        tu.input = nlohmann::json{{"path", "src/main.cpp"}};
        asst_msg.tool_uses.push_back(std::move(tu));
        mixed_messages.push_back(std::move(asst_msg));
    }

    BENCHMARK("estimate_messages_tokens 100 mixed (50% tool_use)") {
        return estimate_messages_tokens(mixed_messages);
    };
}
