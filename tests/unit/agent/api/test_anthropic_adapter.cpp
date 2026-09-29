/**
 * @file test_anthropic_adapter.cpp
 * @brief AnthropicAdapter 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/api/provider/anthropic_adapter.h"

using namespace agent;

TEST_CASE("AnthropicAdapter build_url", "[provider][anthropic]") {
    AnthropicAdapter adapter;

    SECTION("normal base_url") {
        auto url = adapter.build_url("https://api.anthropic.com");
        REQUIRE(url == "https://api.anthropic.com/v1/messages");
    }

    SECTION("base_url with trailing slash") {
        auto url = adapter.build_url("https://api.anthropic.com/");
        REQUIRE(url == "https://api.anthropic.com/v1/messages");
    }

    SECTION("base_url already contains full endpoint") {
        auto url = adapter.build_url("https://api.anthropic.com/v1/messages");
        REQUIRE(url == "https://api.anthropic.com/v1/messages");
    }
}

TEST_CASE("AnthropicAdapter build_headers", "[provider][anthropic]") {
    AnthropicAdapter adapter;

    SECTION("with API key") {
        auto headers = adapter.build_headers("sk-ant-test123");
        REQUIRE(headers.size() >= 3);

        bool has_api_key = false, has_version = false;
        for (const auto& [k, v] : headers) {
            if (k == "x-api-key") {
                REQUIRE(v == "sk-ant-test123");
                has_api_key = true;
            }
            if (k == "anthropic-version") {
                // Phase 1 升级到 2025-01-01 以支持 thinking content block
                REQUIRE(v == "2025-01-01");
                has_version = true;
            }
        }
        REQUIRE(has_api_key);
        REQUIRE(has_version);
    }

    SECTION("without API key still has version header") {
        auto headers = adapter.build_headers("");
        bool has_version = false, has_api_key = false;
        for (const auto& [k, v] : headers) {
            if (k == "x-api-key") has_api_key = true;
            if (k == "anthropic-version") has_version = true;
        }
        REQUIRE_FALSE(has_api_key);
        REQUIRE(has_version);
    }
}

TEST_CASE("AnthropicAdapter build_request_body", "[provider][anthropic]") {
    AnthropicAdapter adapter;

    SECTION("normal request with system prompt") {
        CompletionRequest request;
        request.stream = true;
        request.messages.push_back(ChatMessage::system("You are helpful"));
        request.messages.push_back(ChatMessage::user("Hello"));
        request.messages.push_back(ChatMessage::assistant("Hi"));

        auto body = adapter.build_request_body(request, "claude-sonnet-4");

        // 验证 system 在顶层
        REQUIRE(body.find("\"system\":\"You are helpful\"") != std::string::npos);
        // 验证 model
        REQUIRE(body.find("\"model\":\"claude-sonnet-4\"") != std::string::npos);
        // 验证 stream
        REQUIRE(body.find("\"stream\":true") != std::string::npos);
        // 验证 max_tokens 存在（必填）
        REQUIRE(body.find("\"max_tokens\":") != std::string::npos);
        // 验证 messages 不含 system
        REQUIRE(body.find("\"role\":\"system\"") == std::string::npos);
        // 验证 messages 有 user/assistant
        REQUIRE(body.find("\"role\":\"user\"") != std::string::npos);
        REQUIRE(body.find("\"role\":\"assistant\"") != std::string::npos);
    }

    SECTION("request without system prompt") {
        CompletionRequest request;
        request.stream = true;
        request.messages.push_back(ChatMessage::user("Hello"));

        auto body = adapter.build_request_body(request, "claude-sonnet-4");

        // 无 system 字段
        REQUIRE(body.find("\"system\"") == std::string::npos);
    }

    SECTION("max_tokens uses default when not set") {
        CompletionRequest request;
        request.stream = true;
        request.max_tokens = -1;
        request.messages.push_back(ChatMessage::user("Hi"));

        auto body = adapter.build_request_body(request, "claude-sonnet-4");

        // 应使用默认值 4096
        REQUIRE(body.find("\"max_tokens\":4096") != std::string::npos);
    }
}

TEST_CASE("AnthropicAdapter parse_sse_event", "[provider][anthropic]") {
    AnthropicAdapter adapter;

    SECTION("content_block_delta") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "content_block_delta",
            R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hello"}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.content_delta == "Hello");
        REQUIRE_FALSE(chunk.is_final);
    }

    SECTION("content_block_start with text") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "content_block_start",
            R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":"Hello"}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.content_delta == "Hello");
    }

    SECTION("message_delta with stop_reason") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "message_delta",
            R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":50}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.is_final);
    }

    SECTION("message_stop") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("message_stop", R"({"type":"message_stop"})", chunk);
        REQUIRE(result);
        REQUIRE(chunk.is_final);
    }

    SECTION("message_start event ignored") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "message_start", R"({"type":"message_start","message":{"id":"msg_1"}})", chunk);
        REQUIRE_FALSE(result);
    }

    SECTION("invalid JSON returns false") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("content_block_delta", "not json", chunk);
        REQUIRE_FALSE(result);
    }
}
