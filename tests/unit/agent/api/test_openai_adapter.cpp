/**
 * @file test_openai_adapter.cpp
 * @brief OpenAIAdapter 单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "agent/api/provider/openai_adapter.h"

using namespace agent;

TEST_CASE("OpenAIAdapter build_url", "[provider][openai]") {
    OpenAIAdapter adapter;

    SECTION("normal base_url") {
        auto url = adapter.build_url("https://api.openai.com");
        REQUIRE(url == "https://api.openai.com/v1/chat/completions");
    }

    SECTION("base_url with trailing slash") {
        auto url = adapter.build_url("https://api.deepseek.com/");
        REQUIRE(url == "https://api.deepseek.com/v1/chat/completions");
    }

    SECTION("base_url already contains full endpoint") {
        auto url = adapter.build_url("https://moma.cmecloud.cn/v1/chat/completions");
        REQUIRE(url == "https://moma.cmecloud.cn/v1/chat/completions");
    }

    SECTION("base_url ends with chat/completions (no v1)") {
        auto url = adapter.build_url("https://open.bigmodel.cn/api/paas/v4/chat/completions");
        REQUIRE(url == "https://open.bigmodel.cn/api/paas/v4/chat/completions");
    }
}

TEST_CASE("OpenAIAdapter build_headers", "[provider][openai]") {
    OpenAIAdapter adapter;

    SECTION("with API key") {
        auto headers = adapter.build_headers("sk-test123");
        REQUIRE(headers.size() >= 2);
        bool has_auth = false;
        for (const auto& [k, v] : headers) {
            if (k == "Authorization") {
                REQUIRE(v == "Bearer sk-test123");
                has_auth = true;
            }
        }
        REQUIRE(has_auth);
    }

    SECTION("without API key") {
        auto headers = adapter.build_headers("");
        bool has_auth = false;
        for (const auto& [k, v] : headers) {
            if (k == "Authorization") has_auth = true;
        }
        REQUIRE_FALSE(has_auth);
    }
}

TEST_CASE("OpenAIAdapter build_request_body", "[provider][openai]") {
    OpenAIAdapter adapter;

    CompletionRequest request;
    request.stream = true;
    request.messages.push_back(ChatMessage::system("You are helpful"));
    request.messages.push_back(ChatMessage::user("Hello"));
    request.messages.push_back(ChatMessage::assistant("Hi there!"));
    request.temperature = 0.7f;
    request.max_tokens = 100;

    auto body = adapter.build_request_body(request, "gpt-4o");

    // 验证 JSON 包含关键字段
    REQUIRE(body.find("\"model\":\"gpt-4o\"") != std::string::npos);
    REQUIRE(body.find("\"stream\":true") != std::string::npos);
    REQUIRE(body.find("\"temperature\":") != std::string::npos);
    REQUIRE(body.find("\"max_tokens\":") != std::string::npos);
    REQUIRE(body.find("\"role\":\"system\"") != std::string::npos);
    REQUIRE(body.find("\"role\":\"user\"") != std::string::npos);
    REQUIRE(body.find("\"role\":\"assistant\"") != std::string::npos);
}

TEST_CASE("OpenAIAdapter reasoning_content roundtrip (DS_CACHE P2)",
          "[provider][openai][ds_cache]") {
    SECTION("default: reasoning_content NOT sent") {
        OpenAIAdapter adapter;
        REQUIRE_FALSE(adapter.send_reasoning_content());

        CompletionRequest request;
        request.stream = false;
        auto msg = ChatMessage::assistant("answer");
        msg.reasoning_content = "secret thinking process";
        request.messages.push_back(ChatMessage::user("Q"));
        request.messages.push_back(msg);

        auto body = adapter.build_request_body(request, "deepseek-reasoner");
        // 默认不发送 reasoning_content
        REQUIRE(body.find("reasoning_content") == std::string::npos);
        REQUIRE(body.find("secret thinking process") == std::string::npos);
    }

    SECTION("enabled: reasoning_content sent for assistant role") {
        OpenAIAdapter adapter;
        adapter.set_send_reasoning_content(true);
        REQUIRE(adapter.send_reasoning_content());

        CompletionRequest request;
        request.stream = false;
        auto msg = ChatMessage::assistant("answer");
        msg.reasoning_content = "chain of thought here";
        request.messages.push_back(ChatMessage::user("Q"));
        request.messages.push_back(msg);

        auto body = adapter.build_request_body(request, "deepseek-reasoner");
        // 开启后 reasoning_content 出现在请求体中
        REQUIRE(body.find("reasoning_content") != std::string::npos);
        REQUIRE(body.find("chain of thought here") != std::string::npos);
    }

    SECTION("enabled: empty reasoning_content not sent") {
        OpenAIAdapter adapter;
        adapter.set_send_reasoning_content(true);

        CompletionRequest request;
        request.stream = false;
        request.messages.push_back(ChatMessage::user("Q"));
        request.messages.push_back(ChatMessage::assistant("answer"));  // reasoning_content 为空

        auto body = adapter.build_request_body(request, "deepseek-reasoner");
        // 空 reasoning_content 不发送字段
        REQUIRE(body.find("reasoning_content") == std::string::npos);
    }

    SECTION("enabled: user/tool role reasoning_content not sent") {
        OpenAIAdapter adapter;
        adapter.set_send_reasoning_content(true);

        CompletionRequest request;
        request.stream = false;
        auto user_msg = ChatMessage::user("Q");
        user_msg.reasoning_content = "should not appear";  // user 角色不应发送
        request.messages.push_back(user_msg);

        auto body = adapter.build_request_body(request, "deepseek-reasoner");
        REQUIRE(body.find("should not appear") == std::string::npos);
    }
}

TEST_CASE("OpenAIAdapter multimodal image content", "[provider][openai][vision]") {
    OpenAIAdapter adapter;

    // 临时小图片文件（内容无关紧要，仅验证 base64 data URI 与 mime）
    auto img_path = std::filesystem::temp_directory_path() / "workx_test_img.png";
    {
        std::ofstream f(img_path, std::ios::binary);
        f << "hello";
    }

    CompletionRequest request;
    request.stream = false;
    request.messages.push_back(ChatMessage::user("图里有什么?", {img_path.string()}));

    auto body = adapter.build_request_body(request, "qwen2.5-vl-72b-instruct");

    // content 序列化为块数组
    REQUIRE(body.find("\"content\":[{") != std::string::npos);
    // 文本块
    REQUIRE(body.find("\"type\":\"text\"") != std::string::npos);
    REQUIRE(
        body.find("\"text\":\"\xe5\x9b\xbe\xe9\x87\x8c\xe6\x9c\x89\xe4\xbb\x80\xe4\xb9\x88?\"") !=
        std::string::npos);
    // 图片块：base64 data URI（"hello" → aGVsbG8=）
    REQUIRE(body.find("\"type\":\"image_url\"") != std::string::npos);
    REQUIRE(body.find("\"url\":\"data:image/png;base64,aGVsbG8=\"") != std::string::npos);

    std::filesystem::remove(img_path);
}

TEST_CASE("OpenAIAdapter multimodal skips missing image file", "[provider][openai][vision]") {
    OpenAIAdapter adapter;

    CompletionRequest request;
    request.stream = false;
    request.messages.push_back(ChatMessage::user(
        "图里有什么?",
        {std::filesystem::temp_directory_path().string() + "/workx_missing_img.png"}));

    auto body = adapter.build_request_body(request, "qwen2.5-vl-72b-instruct");

    // 图片读取失败被跳过：退回纯文本 content（无块数组）
    REQUIRE(body.find("image_url") == std::string::npos);
    REQUIRE(body.find("图里有什么?") != std::string::npos);
}

TEST_CASE("OpenAIAdapter content stays plain string without images", "[provider][openai]") {
    OpenAIAdapter adapter;

    CompletionRequest request;
    request.stream = false;
    request.messages.push_back(ChatMessage::user("Hello"));
    auto body = adapter.build_request_body(request, "gpt-4o");

    REQUIRE(body.find("\"content\":\"Hello\"") != std::string::npos);
    REQUIRE(body.find("image_url") == std::string::npos);
}

TEST_CASE("OpenAIAdapter parse_sse_event", "[provider][openai]") {
    OpenAIAdapter adapter;

    SECTION("content delta") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "", R"({"choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.content_delta == "Hello");
        REQUIRE_FALSE(chunk.is_final);
    }

    SECTION("finish reason") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "",
            R"({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":20}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.is_final);
    }

    SECTION("usage chunk with OpenAI cached_tokens") {
        // OpenAI 标准缓存字段：usage.prompt_tokens_details.cached_tokens
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "",
            R"({"choices":[],"usage":{"prompt_tokens":1000,"completion_tokens":50,
                "prompt_tokens_details":{"cached_tokens":900}}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.prompt_tokens == 1000);
        REQUIRE(chunk.generated_tokens == 50);
        REQUIRE(chunk.prompt_cache_hit_tokens == 900);
        REQUIRE(chunk.prompt_cache_miss_tokens == 100);
    }

    SECTION("usage chunk with deepseek private cache fields") {
        // DeepSeek 官方私有字段优先
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "",
            R"({"choices":[],"usage":{"prompt_tokens":1000,"completion_tokens":50,
                "prompt_cache_hit_tokens":700,"prompt_cache_miss_tokens":300}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.prompt_cache_hit_tokens == 700);
        REQUIRE(chunk.prompt_cache_miss_tokens == 300);
    }

    SECTION("usage chunk without cache fields") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "", R"({"choices":[],"usage":{"prompt_tokens":1000,"completion_tokens":50}})", chunk);
        REQUIRE(result);
        REQUIRE(chunk.prompt_cache_hit_tokens == 0);
        REQUIRE(chunk.prompt_cache_miss_tokens == 0);
    }

    SECTION("[DONE] signal") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("", "[DONE]", chunk);
        REQUIRE(result);
        REQUIRE(chunk.is_final);
    }

    SECTION("reasoning content (deepseek format)") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "",
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"thinking..."},"finish_reason":null}]})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.reasoning_delta == "thinking...");
    }

    SECTION("empty delta returns false") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event(
            "", R"({"choices":[{"index":0,"delta":{},"finish_reason":null}]})", chunk);
        REQUIRE_FALSE(result);
    }

    SECTION("invalid JSON returns false") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("", "not json", chunk);
        REQUIRE_FALSE(result);
    }
}
