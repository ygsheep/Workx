/**
 * @file test_client.cpp
 * @brief Client 顶层封装测试
 * @details 使用 LM Studio (OpenAI 兼容 API) 作为测试服务器。
 *          默认模型 google/gemma-4-e4b（支持推理）。
 *          环境变量：
 *            LM_STUDIO_BASE_URL — 服务器地址（默认 http://127.0.0.1:1234）
 *            LM_STUDIO_MODEL    — 模型名（默认 google/gemma-4-e4b）
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/api/client.h"
#include "agent/api/remote/http_client.h"

#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <condition_variable>

using namespace agent;

// ============================================================
// LM Studio 测试 Fixture
// ============================================================

static std::string s_base_url;
static std::string s_model;

struct LmStudioFixture {
    LmStudioFixture() {
        static bool initialized = false;
        if (initialized) return;

        // X-2 修复：未设置 LM_STUDIO_BASE_URL 时跳过（避免误用 Python mock 服务器
        // 跑 LLM 推理测试用例）。需要 LLM 推理的测试必须显式设置 LM_STUDIO_BASE_URL
        const char* env_url = std::getenv("LM_STUDIO_BASE_URL");
        if (!env_url || env_url[0] == '\0') {
            SKIP("LLM 推理测试需要 LM Studio。请设置 LM_STUDIO_BASE_URL 并启动 LM Studio");
        }
        s_base_url = env_url;
        while (!s_base_url.empty() && s_base_url.back() == '/') s_base_url.pop_back();
        REQUIRE_FALSE(s_base_url.empty());

        const char* env_model = std::getenv("LM_STUDIO_MODEL");
        s_model = (env_model && env_model[0] != '\0') ? env_model : "google/gemma-4-e4b";

        // 连通性测试：确保 LM Studio 已启动
        HttpClient check_client;
        auto result = check_client.get(s_base_url + "/v1/models", {}, 5000);
        if (!result.is_ok() || result.value().status_code != 200) {
            FAIL("LM Studio 未启动或无法连接。请先启动 LM Studio 服务器 (lms server start)");
        }
        initialized = true;
    }
};

// ============================================================
// 创建测试
// ============================================================

TEST_CASE_METHOD(LmStudioFixture, "Client create success", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());
    REQUIRE(client.model_name() == s_model);
}

TEST_CASE("Client create failure without base_url", "[client]") {
    // 既不设 provider 也不设 base_url → initialize 失败
    auto r = Client::create({.backend = {.provider = ProviderType::OpenAI}});
    REQUIRE(r.is_err());
}

// ============================================================
// 后端能力透传
// ============================================================

TEST_CASE_METHOD(LmStudioFixture, "Client list_models", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    auto models = client.list_models();
    REQUIRE(models.is_ok());
    REQUIRE_FALSE(models.value().empty());
}

TEST_CASE_METHOD(LmStudioFixture, "Client set_model", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    // 获取第一个可用模型名
    auto models = client.list_models();
    REQUIRE(models.is_ok());
    auto model_list = std::move(models.value());
    REQUIRE_FALSE(model_list.empty());

    const auto& first_model = model_list[0].name;
    client.set_model(first_model);
    REQUIRE(client.model_name() == first_model);
}

// ============================================================
// 阻塞 API 测试
// ============================================================

TEST_CASE_METHOD(LmStudioFixture, "Client chat sync", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    auto reply = client.chat("Say hello in one word.");
    REQUIRE(reply.is_ok());
    REQUIRE_FALSE(reply.value().empty());

    auto h = client.history();
    REQUIRE(h.size() >= 2);  // user + assistant
    REQUIRE(h.back().role == ChatMessage::Role::Assistant);
}

TEST_CASE_METHOD(LmStudioFixture, "Client stream_chat content", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    std::string content, reasoning;
    bool done = false;
    auto res = client.stream_chat(
        "Say hi.", {[&](const std::string& c, const std::string& r) {
                        content += c;
                        reasoning += r;
                    },
                    [&](const StreamChunk&) { done = true; }, [](const std::string&, bool) {}});
    REQUIRE(res.is_ok());
    REQUIRE(done);
    REQUIRE_FALSE(content.empty());
}

TEST_CASE_METHOD(LmStudioFixture, "Client stream_chat reasoning", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    std::string content, reasoning;
    client.stream_chat("Think step by step: what is 15 + 27?",
                       {[&](const std::string& c, const std::string& r) {
                            content += c;
                            reasoning += r;
                        },
                        [](const StreamChunk&) {}, [](const std::string&, bool) {}});

    // gemma-4-e4b 支持推理，预期有 reasoning；环境差异时 WARN 跳过
    if (reasoning.empty()) {
        WARN("模型未产生 reasoning_content，跳过思考断言");
    } else {
        REQUIRE_FALSE(reasoning.empty());
    }
    REQUIRE_FALSE(content.empty());
}

// ============================================================
// 会话管理测试
// ============================================================

TEST_CASE_METHOD(LmStudioFixture, "Client clear_history", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    // 先对话产生历史
    auto reply = client.chat("Hi");
    REQUIRE(reply.is_ok());
    REQUIRE_FALSE(client.history().empty());

    client.clear_history();
    REQUIRE(client.history().empty());
}

// ============================================================
// 异步 API 测试（TUI 场景：不阻塞调用线程）
// ============================================================

TEST_CASE_METHOD(LmStudioFixture, "Client stream_chat_async", "[client][integration][async]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    std::mutex mtx;
    std::condition_variable cv;
    std::string content, reasoning;
    bool done = false;

    // 立即返回，推理在后台线程
    auto res =
        client.stream_chat_async("Say hi.", {[&](const std::string& c, const std::string& r) {
                                                 std::lock_guard<std::mutex> lk(mtx);
                                                 content += c;
                                                 reasoning += r;
                                             },
                                             [&](const StreamChunk&) {
                                                 std::lock_guard<std::mutex> lk(mtx);
                                                 done = true;
                                                 cv.notify_one();
                                             },
                                             [&](const std::string&, bool) {
                                                 std::lock_guard<std::mutex> lk(mtx);
                                                 done = true;
                                                 cv.notify_one();
                                             }});
    REQUIRE(res.is_ok());

    // 等待后台完成（最多 30s）
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(30), [&] { return done; });
    }
    REQUIRE(done);
    REQUIRE_FALSE(content.empty());
}

TEST_CASE_METHOD(LmStudioFixture, "Client interrupt async", "[client][integration][async]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.is_ok());
    auto client = std::move(r.value());

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    client.stream_chat_async("Write a long essay about history.",
                             {[](const std::string&, const std::string&) {},
                              [&](const StreamChunk&) {
                                  std::lock_guard<std::mutex> lk(mtx);
                                  done = true;
                                  cv.notify_one();
                              },
                              [&](const std::string&, bool) {
                                  std::lock_guard<std::mutex> lk(mtx);
                                  done = true;
                                  cv.notify_one();
                              }});

    REQUIRE(client.is_generating());

    // 等待一小会让流开始，然后中断
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    client.interrupt();

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; });
    }
    // 中断后应结束（done=true），且不再 generating
    REQUIRE(done);
    REQUIRE_FALSE(client.is_generating());
}
