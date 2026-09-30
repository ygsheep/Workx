/**
 * @file test_chat_session.cpp
 * @brief ChatSession 单元测试
 * @details H-A/H-B：修复 Closed AI Loop 与单例污染问题
 *          - H-A：serialize_state 测试不再通过 deserialize_state 设置状态，
 *                 改用 commit_state 直接注入
 *          - H-B：所有测试改用 MockConfigManager，避免污染单例
 */

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <thread>
#include "agent/core/chat_session.h"
#include "agent/session/session_store.h"  // 项目会话恢复
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "agent/message/types.h"
#include "agent/api/retry.h"
#include "helpers/mock_provider.h"
#include "helpers/mock_config_manager.h"  // H-B

using namespace agent;
using namespace agent::test;

namespace {

/// @brief 创建测试用 ChatSession（H-B：使用 MockConfigManager）
std::unique_ptr<ChatSession> make_test_session(MockConfigManager& cfg) {
    return std::make_unique<ChatSession>(
        std::unique_ptr<ICompletionProvider>(new test::MockCompletionProvider()),
        TaskManager::instance(), EventBus::instance(), cfg);
}

/// @brief 可阻塞的流式读取器：next() 挂起直到 release()，用于制造"模型忙碌"窗口
/// @details 继承 MockStreamReader 以便传给 MockCompletionProvider::set_next_reader
class BlockingStreamReader : public MockStreamReader {
   public:
    void release() {
        {
            std::lock_guard<std::mutex> lock(m_);
            released_ = true;
        }
        cv_.notify_all();
    }

    StreamState next(std::function<bool()> /*should_stop*/, StreamChunk& out) override {
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return released_; });
        }
        out = StreamChunk{};
        out.is_final = true;
        out.prompt_tokens = 10;
        out.generated_tokens = 5;
        return StreamState::Complete;
    }

    void cancel() override {}

   private:
    std::mutex m_;
    std::condition_variable cv_;
    bool released_ = false;
};

}  // anonymous namespace

TEST_CASE("ChatSession basic operations", "[session]") {
    MockConfigManager cfg;

    auto session = make_test_session(cfg);

    REQUIRE_FALSE(session->is_generating());
    REQUIRE(session->get_messages().empty());

    session->set_system_prompt("You are helpful");
    session->clear_history();
}

TEST_CASE("ChatSession load non-existent file", "[session]") {
    MockConfigManager cfg;

    auto session = make_test_session(cfg);

    auto load_result = session->load_session("nonexistent_file_12345.json");
    REQUIRE(load_result.isErr());
}

TEST_CASE("ChatSession save and load round-trip", "[session]") {
    MockConfigManager cfg;

    std::string test_path = "test_roundtrip_tmp.json";

    {
        auto session = make_test_session(cfg);
        session->set_system_prompt("Test system prompt");

        auto save_result = session->save_session(test_path);
        REQUIRE(save_result.isOk());
    }

    {
        auto session = make_test_session(cfg);

        auto load_result = session->load_session(test_path);
        REQUIRE(load_result.isOk());
    }

    std::filesystem::remove(test_path);
}

// ============================================================
// H-6: serialize_state / deserialize_state 纯函数测试
// ============================================================

TEST_CASE("ChatSession serialize_state empty messages", "[session][h6][serialize]") {
    MockConfigManager cfg;

    auto session = make_test_session(cfg);
    // 不设置 system_prompt，不添加 messages
    auto j = session->serialize_state();

    // 空 system_prompt 不应出现在 JSON 中
    REQUIRE_FALSE(j.contains("system_prompt"));
    // messages 应为空数组
    REQUIRE(j.contains("messages"));
    REQUIRE(j["messages"].is_array());
    REQUIRE(j["messages"].empty());
}

TEST_CASE("ChatSession serialize_state with system_prompt", "[session][h6][serialize]") {
    MockConfigManager cfg;

    auto session = make_test_session(cfg);
    session->set_system_prompt("You are helpful");

    auto j = session->serialize_state();
    REQUIRE(j["system_prompt"] == "You are helpful");
}

TEST_CASE("ChatSession serialize_state all roles", "[session][h6][serialize]") {
    MockConfigManager cfg;

    auto session = make_test_session(cfg);

    // H-A 修复：原测试通过 deserialize_state 设置状态后用 serialize_state 验证，
    // 形成 Closed AI Loop（deserialize 错误会掩盖 serialize 错误）。
    // 改用 commit_state 直接注入已知状态。
    std::vector<ChatMessage> messages;
    messages.push_back({.role = ChatMessage::Role::System, .content = "sys msg"});
    messages.push_back({.role = ChatMessage::Role::User, .content = "hello"});
    messages.push_back(
        {.role = ChatMessage::Role::Assistant, .content = "hi", .reasoning_content = "thinking"});
    messages.push_back({.role = ChatMessage::Role::Tool,
                        .content = "result",
                        .tool_call_id = "tc1",
                        .tool_name = "Read"});
    session->commit_state(std::move(messages), "sys");

    auto j = session->serialize_state();
    REQUIRE(j["system_prompt"] == "sys");
    REQUIRE(j["messages"].size() == 4);

    // 验证字段顺序与内容
    REQUIRE(j["messages"][0]["role"] == "system");
    REQUIRE(j["messages"][0]["content"] == "sys msg");

    REQUIRE(j["messages"][1]["role"] == "user");
    REQUIRE(j["messages"][1]["content"] == "hello");

    REQUIRE(j["messages"][2]["role"] == "assistant");
    REQUIRE(j["messages"][2]["content"] == "hi");
    REQUIRE(j["messages"][2]["reasoning_content"] == "thinking");

    REQUIRE(j["messages"][3]["role"] == "tool");
    REQUIRE(j["messages"][3]["content"] == "result");
    REQUIRE(j["messages"][3]["tool_call_id"] == "tc1");
    REQUIRE(j["messages"][3]["tool_name"] == "Read");
}

TEST_CASE("ChatSession deserialize_state round-trip", "[session][h6][deserialize]") {
    MockConfigManager cfg;

    // H-A 修复：直接测试 deserialize_state 纯函数本身，不依赖 session 实例
    nlohmann::json input;
    input["system_prompt"] = "round-trip test";
    input["messages"] = nlohmann::json::array(
        {{{"role", "user"}, {"content", "q1"}}, {{"role", "assistant"}, {"content", "a1"}}});

    auto parse_result = ChatSession::deserialize_state(input);
    REQUIRE(parse_result.isOk());

    auto [messages, system_prompt] = std::move(parse_result).unwrap();
    REQUIRE(system_prompt == "round-trip test");
    REQUIRE(messages.size() == 2);
    REQUIRE(messages[0].role == ChatMessage::Role::User);
    REQUIRE(messages[0].content == "q1");
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "a1");

    // 通过 commit_state 提交后再 serialize 验证 round-trip
    auto session = make_test_session(cfg);
    session->commit_state(std::vector<ChatMessage>(messages), system_prompt);

    auto j = session->serialize_state();
    REQUIRE(j["system_prompt"] == "round-trip test");
    REQUIRE(j["messages"].size() == 2);
    REQUIRE(j["messages"][0]["role"] == "user");
    REQUIRE(j["messages"][0]["content"] == "q1");
}

TEST_CASE("ChatSession deserialize_state rejects unknown role", "[session][h6][deserialize]") {
    nlohmann::json input;
    input["messages"] = nlohmann::json::array({{{"role", "alien"}, {"content", "??"}}});

    auto r = ChatSession::deserialize_state(input);
    REQUIRE(r.isErr());
    REQUIRE(r.error().find("Unknown role") != std::string::npos);
}

TEST_CASE("ChatSession deserialize_state accepts missing messages", "[session][h6][deserialize]") {
    nlohmann::json input;  // 完全空对象
    auto r = ChatSession::deserialize_state(input);
    REQUIRE(r.isOk());
    REQUIRE(r.unwrap().first.empty());  // messages 为空
}

TEST_CASE("ChatSession deserialize_state rejects malformed content", "[session][h6][deserialize]") {
    // content 字段缺失
    nlohmann::json input;
    input["messages"] = nlohmann::json::array({
        nlohmann::json::object({{"role", "user"}})  // 缺 content
    });

    auto r = ChatSession::deserialize_state(input);
    REQUIRE(r.isErr());
}

// ============================================================
// H-7: compute_retry 纯函数测试
// ============================================================

TEST_CASE("compute_retry returns Continue when no error", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = false;
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Continue);
    REQUIRE(d.delay_ms == 0);
}

TEST_CASE("compute_retry returns Sleep when retryable", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "connection timeout";  // 网络错误，可重试
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    // attempt=0: delay = 1000 * 2^0 = 1000
    auto d0 = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d0.action == RetryAction::Sleep);
    REQUIRE(d0.delay_ms == 1000);

    // attempt=1: delay = 1000 * 2^1 = 2000
    auto d1 = ChatSession::compute_retry(r, policy, 1);
    REQUIRE(d1.action == RetryAction::Sleep);
    REQUIRE(d1.delay_ms == 2000);

    // attempt=2: delay = 1000 * 2^2 = 4000
    auto d2 = ChatSession::compute_retry(r, policy, 2);
    REQUIRE(d2.action == RetryAction::Sleep);
    REQUIRE(d2.delay_ms == 4000);
}

// ============================================================
// #89：compute_retry 依据真实 HTTP 状态码判定（此前硬编码 http_status=0）
// ============================================================

TEST_CASE("compute_retry retries on http 429", "[session][h7][retry][89]") {
    ReActResult r;
    r.was_error = true;
    r.http_status = 429;
    r.error_message = "HTTP error: 429";
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Sleep);
    REQUIRE(d.delay_ms == 1000);
}

TEST_CASE("compute_retry retries on http 503", "[session][h7][retry][89]") {
    ReActResult r;
    r.was_error = true;
    r.http_status = 503;
    r.error_message = "HTTP error: 503";
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Sleep);
}

TEST_CASE("compute_retry stops on http 4xx instead of retrying", "[session][h7][retry][89]") {
    // 修复前：http_status 恒传 0，is_retryable(0, 非空 msg) 恒 true —— 400/401 也会重试。
    // 客户端错误重试无益（也是后续 fallback 判定「换模型有没有用」的依据）。
    for (int status : {400, 401, 403, 404}) {
        ReActResult r;
        r.was_error = true;
        r.http_status = status;
        r.error_message = "HTTP error: " + std::to_string(status);
        HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

        auto d = ChatSession::compute_retry(r, policy, 0);
        REQUIRE(d.action == RetryAction::Stop);
    }
}

TEST_CASE("compute_retry treats http_status 0 as network error", "[session][h7][retry][89]") {
    // 无 HTTP 响应（连接超时 / 提交失败）：仍按网络错误可重试，与修复前一致
    ReActResult r;
    r.was_error = true;
    r.http_status = 0;
    r.error_message = "Connection timed out";
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Sleep);
}

TEST_CASE("compute_retry returns Stop when attempts exhausted", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "connection timeout";
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    // attempt == max_retries: 不可重试
    auto d = ChatSession::compute_retry(r, policy, 3);
    REQUIRE(d.action == RetryAction::Stop);
    REQUIRE(d.delay_ms == 0);

    // attempt > max_retries: 同样不可重试
    auto d2 = ChatSession::compute_retry(r, policy, 5);
    REQUIRE(d2.action == RetryAction::Stop);
}

TEST_CASE("compute_retry returns Stop for non-retryable error", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "max iterations reached";  // 业务错误，不可重试
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Stop);
    REQUIRE(d.delay_ms == 0);
}

TEST_CASE("compute_retry respects max_delay_ms cap", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "503 service unavailable";
    HttpRetryPolicy policy{.max_retries = 30, .base_delay_ms = 1000, .max_delay_ms = 60000};

    // attempt=10: 1000 * 2^10 = 102400 > 60000 → 截断到 60000
    auto d = ChatSession::compute_retry(r, policy, 10);
    REQUIRE(d.action == RetryAction::Sleep);
    REQUIRE(d.delay_ms == 60000);
}

TEST_CASE("compute_retry handles empty error_message", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "";  // 空 message：http_status=0 且 msg 为空 → 不可重试
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Stop);
}

// C-3 回归测试：attempt >= 63 不应触发 UB
TEST_CASE("compute_retry handles attempt >= 63 without UB", "[session][h7][retry][c-3]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "connection timeout";
    HttpRetryPolicy policy{.max_retries = 100, .base_delay_ms = 1000, .max_delay_ms = 60000};

    // attempt=63: 1LL << 63 是 UB（有符号 int64），应被保护直接返回 max_delay_ms
    auto d = ChatSession::compute_retry(r, policy, 63);
    REQUIRE(d.action == RetryAction::Sleep);
    REQUIRE(d.delay_ms == 60000);

    // attempt=100: 超过 max_retries，Stop
    auto d2 = ChatSession::compute_retry(r, policy, 100);
    REQUIRE(d2.action == RetryAction::Stop);
}

// ============================================================
// H-8: 验证 ChatSession 不再暴露 backend()
// ============================================================

// 注：H-8 删除了 ChatSession::backend() 方法。若代码回退恢复该方法，
// 以下编译期 static_assert 会立即失败（&ChatSession::backend 在成员不存在时
// 是 ill-formed，编译报错）。这是预期的——H-8 的"测试"主要体现在
// SessionResult.backend_admin 字段的填充与使用上（见 test_factory.cpp）。
// 此处仅保留文档性注释，无 runtime 测试用例。

// ============================================================
// 项目会话恢复：serialize_state/deserialize_state tool_uses round-trip
// ============================================================

TEST_CASE("ChatSession serialize/deserialize tool_uses round-trip",
          "[session][restore][serialize]") {
    // 构造含 tool_uses 的 assistant 消息
    nlohmann::json input;
    input["system_prompt"] = "test";
    nlohmann::json uses = nlohmann::json::array(
        {{{"id", "toolu_001"}, {"name", "Read"}, {"input", {{"path", "/tmp/test"}}}},
         {{"id", "toolu_002"},
          {"name", "Write"},
          {"input", {{"path", "/tmp/out"}, {"content", "hi"}}}}});
    input["messages"] = nlohmann::json::array({{{"role", "user"}, {"content", "read and write"}},
                                               {{"role", "assistant"},
                                                {"content", "calling tools"},
                                                {"reasoning_content", "thinking"},
                                                {"tool_uses", uses}},
                                               {{"role", "tool"},
                                                {"content", "ok"},
                                                {"tool_call_id", "toolu_001"},
                                                {"tool_name", "Read"},
                                                {"is_error", true}}});

    // 反序列化
    auto parse_result = ChatSession::deserialize_state(input);
    REQUIRE(parse_result.isOk());
    auto [messages, system_prompt] = std::move(parse_result).unwrap();

    REQUIRE(messages.size() == 3);
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].tool_uses.size() == 2);
    REQUIRE(messages[1].tool_uses[0].id == "toolu_001");
    REQUIRE(messages[1].tool_uses[0].name == "Read");
    REQUIRE(messages[1].tool_uses[0].input["path"] == "/tmp/test");
    REQUIRE(messages[1].tool_uses[1].name == "Write");

    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].is_error == true);

    // 再序列化验证 round-trip
    MockConfigManager cfg;
    auto session = make_test_session(cfg);
    session->commit_state(std::vector<ChatMessage>(messages), system_prompt);
    auto j = session->serialize_state();

    REQUIRE(j["messages"][1]["tool_uses"].size() == 2);
    REQUIRE(j["messages"][1]["tool_uses"][0]["id"] == "toolu_001");
    REQUIRE(j["messages"][1]["tool_uses"][1]["name"] == "Write");
    REQUIRE(j["messages"][2]["is_error"] == true);
}

// ============================================================
// 项目会话恢复：ChatSession + SessionStore 集成
// ============================================================

TEST_CASE("ChatSession restore_from_file loads messages", "[session][restore]") {
    MockConfigManager cfg;
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "workx_test_restore_chat.jsonl";
    fs::remove(tmp);

    // 写入测试数据
    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/tmp","model":"m","gitBranch":"main"})"
          << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hello"})"
          << "\n";
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","timestamp":"t2","content":"hi","reasoningContent":"thinking","toolUses":[]})"
          << "\n";
        f << R"({"type":"tool","uuid":"t1","parentUuid":"a1","timestamp":"t3","toolCallId":"tc1","toolName":"Read","content":"file","isError":false})"
          << "\n";
    }

    auto session = make_test_session(cfg);
    REQUIRE(session->restore_from_file(tmp.string()));

    auto messages = session->get_messages();
    REQUIRE(messages.size() == 3);
    REQUIRE(messages[0].role == ChatMessage::Role::User);
    REQUIRE(messages[0].content == "hello");
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "hi");
    REQUIRE(messages[1].reasoning_content == "thinking");
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].tool_call_id == "tc1");
    REQUIRE(messages[2].tool_name == "Read");

    fs::remove(tmp);
}

TEST_CASE("ChatSession restore_from_file empty file returns false", "[session][restore]") {
    MockConfigManager cfg;
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "workx_test_restore_empty.jsonl";
    fs::remove(tmp);

    // 创建空文件
    std::ofstream(tmp.string()) << "";

    auto session = make_test_session(cfg);
    REQUIRE_FALSE(session->restore_from_file(tmp.string()));

    fs::remove(tmp);
}

TEST_CASE("ChatSession set_session_store and session_store accessor", "[session][restore]") {
    MockConfigManager cfg;
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "workx_test_set_store.jsonl";
    fs::remove(tmp);

    auto session = make_test_session(cfg);

    // 初始状态 session_store() 为 nullptr
    REQUIRE(session->session_store() == nullptr);

    // 创建并注入 SessionStore
    auto store = std::make_shared<agent::session::SessionStore>(tmp.string(), "test-id");
    REQUIRE(store->open());
    store->append_session_start("/cwd", "test-model", "main");
    session->set_session_store(store);

    // 验证 session_store() 返回注入的 store
    REQUIRE(session->session_store() != nullptr);
    REQUIRE(session->session_store() == store);

    store->append_session_end();
    store->close();

    // 验证文件内容
    auto events = agent::session::SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);  // session_start + session_end
    REQUIRE(events[0]["type"] == "session_start");
    REQUIRE(events[0]["sessionId"] == "test-id");
    REQUIRE(events[1]["type"] == "session_end");

    fs::remove(tmp);
}

TEST_CASE("ChatSession new_session clears messages and switches id", "[session][new_session]") {
    MockConfigManager cfg;
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "workx_test_new_session.jsonl";
    fs::remove(tmp);

    auto session = make_test_session(cfg);
    const std::string old_id = session->session_id();
    REQUIRE_FALSE(old_id.empty());

    // 注入消息 + SessionStore（模拟已有会话）
    session->commit_state({ChatMessage::user("hello"), ChatMessage::assistant("hi")}, "sys");
    auto store = std::make_shared<agent::session::SessionStore>(tmp.string(), old_id);
    REQUIRE(store->open());
    store->append_session_start("/cwd", "model", "main");
    store->append_user_message("u1", "", "hello", "t1");
    store->close();
    session->set_session_store(store);
    REQUIRE(session->get_messages().size() == 2);

    // new_session：清空消息 + 换 id + 关闭 store（新会话文件懒创建）
    session->new_session();
    REQUIRE(session->get_messages().empty());
    REQUIRE_FALSE(session->session_id().empty());
    REQUIRE(session->session_id() != old_id);
    REQUIRE(session->session_store() == nullptr);

    // 旧会话文件保留（/new 语义：不删除，由 /clear 调用方删除）
    REQUIRE(fs::exists(tmp));

    fs::remove(tmp);
}

TEST_CASE("ChatSession full restore cycle: write then restore", "[session][restore][e2e]") {
    // 端到端测试：通过 SessionStore 写入消息 → ChatSession 从文件恢复 → 验证消息一致
    MockConfigManager cfg;
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "workx_test_e2e_cycle.jsonl";
    fs::remove(tmp);

    // 写入测试数据（模拟一个完整的会话生命周期）
    {
        auto store = std::make_shared<agent::session::SessionStore>(tmp.string(), "e2e-id");
        REQUIRE(store->open());
        store->append_session_start("/project", "model-x", "develop");
        store->append_user_message("u1", "", "question 1", "t1");
        store->append_assistant_message("a1", "u1", "answer 1", "thinking", {}, "t2");
        store->append_user_message("u2", "a1", "question 2", "t3");
        store->append_session_end();
        store->close();
    }

    // 从文件恢复
    auto session = make_test_session(cfg);
    REQUIRE(session->restore_from_file(tmp.string()));

    auto restored = session->get_messages();
    REQUIRE(restored.size() == 3);  // user + assistant + user（不含 session_start/end）
    REQUIRE(restored[0].role == ChatMessage::Role::User);
    REQUIRE(restored[0].content == "question 1");
    REQUIRE(restored[1].role == ChatMessage::Role::Assistant);
    REQUIRE(restored[1].content == "answer 1");
    REQUIRE(restored[1].reasoning_content == "thinking");
    REQUIRE(restored[2].role == ChatMessage::Role::User);
    REQUIRE(restored[2].content == "question 2");

    fs::remove(tmp);
}

// ============================================================
// /provider 热切换链路：get_messages → import_messages 保留当前对话继续
// ============================================================

TEST_CASE("ChatSession import_messages keeps conversation across provider switch",
          "[session][import]") {
    MockConfigManager cfg;

    // 旧 session：注入一段多模态对话（user 带图片 + assistant 回复）
    std::vector<ChatMessage> original;
    original.push_back(ChatMessage::user("看图说话", {"C:/tmp/a.png", "C:/tmp/b.png"}));
    original.push_back(ChatMessage::assistant("图片中有两只猫"));

    auto old_session = make_test_session(cfg);
    old_session->commit_state(original, "system prompt");

    // 热切换步骤1：备份当前消息
    std::vector<ChatMessage> backup = old_session->get_messages();
    REQUIRE(backup.size() == 2);

    // 热切换步骤2：新 session 导入备份（清空自身消息后填入）
    auto new_session = make_test_session(cfg);
    new_session->set_system_prompt("old prompt");
    new_session->commit_state({ChatMessage::user("旧消息占位")}, "old prompt");
    new_session->import_messages(std::move(backup));

    // 验证：消息完整保留（含图片路径），且替换而非追加
    auto imported = new_session->get_messages();
    REQUIRE(imported.size() == 2);
    REQUIRE(imported[0].role == ChatMessage::Role::User);
    REQUIRE(imported[0].content == "看图说话");
    REQUIRE(imported[0].image_paths.size() == 2);
    REQUIRE(imported[0].image_paths[0] == "C:/tmp/a.png");
    REQUIRE(imported[0].image_paths[1] == "C:/tmp/b.png");
    REQUIRE(imported[1].role == ChatMessage::Role::Assistant);
    REQUIRE(imported[1].content == "图片中有两只猫");

    // 导入空消息：清空历史（热切换时空对话场景）
    new_session->import_messages({});
    REQUIRE(new_session->get_messages().empty());
}

// ============================================================
// 重试按钮：regenerate_from 截断到指定用户消息并重新生成
// ============================================================

TEST_CASE("ChatSession regenerate_from truncates to matching user message",
          "[session][retry][regenerate]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    // 注入多轮对话：q1/a1、q2/a2
    std::vector<ChatMessage> history;
    history.push_back(ChatMessage::user("q1"));
    history.push_back(ChatMessage::assistant("a1"));
    history.push_back(ChatMessage::user("q2"));
    history.push_back(ChatMessage::assistant("a2"));
    session->commit_state(std::move(history), "sys");

    // 为重新生成排队一个 mock reader
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("a1'");
    auto* provider = static_cast<MockCompletionProvider*>(session->completion_provider());
    provider->set_next_reader(reader);

    // 重试 q1 的回复：截断到 q1（含删除），重新生成
    session->regenerate_from("q1");
    TaskManager::instance().waitForAll();

    // 验证：q1 之后全部删除，run_completion 重新 push q1 + 新回复
    auto msgs = session->get_messages();
    REQUIRE(msgs.size() == 2);
    REQUIRE(msgs[0].role == ChatMessage::Role::User);
    REQUIRE(msgs[0].content == "q1");
    REQUIRE(msgs[1].role == ChatMessage::Role::Assistant);
    REQUIRE(msgs[1].content == "a1'");
    REQUIRE(provider->submit_count == 1);
}

TEST_CASE("ChatSession regenerate_from keeps later turns when retrying last reply",
          "[session][retry][regenerate]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    std::vector<ChatMessage> history;
    history.push_back(ChatMessage::user("q1"));
    history.push_back(ChatMessage::assistant("a1"));
    history.push_back(ChatMessage::user("q2"));
    history.push_back(ChatMessage::assistant("a2"));
    session->commit_state(std::move(history), "sys");

    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("a2'");
    auto* provider = static_cast<MockCompletionProvider*>(session->completion_provider());
    provider->set_next_reader(reader);

    // 重试最后一条回复（q2）：仅删除 q2/a2，保留 q1/a1
    session->regenerate_from("q2");
    TaskManager::instance().waitForAll();

    auto msgs = session->get_messages();
    REQUIRE(msgs.size() == 4);
    REQUIRE(msgs[0].content == "q1");
    REQUIRE(msgs[1].content == "a1");
    REQUIRE(msgs[2].role == ChatMessage::Role::User);
    REQUIRE(msgs[2].content == "q2");
    REQUIRE(msgs[3].role == ChatMessage::Role::Assistant);
    REQUIRE(msgs[3].content == "a2'");
}

// ============================================================
// #45：会话级权限两态切换（Shift+Tab：Default ↔ Bypass，计划模式独立为工作模式）
// ============================================================

TEST_CASE("ChatSession permission mode two-state toggle", "[session][permission][45]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    // 初始态：Default
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);

    // Default → Bypass
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);

    // Bypass → Default
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);

    // 循环稳定：连续一轮回到 Bypass
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);
}

TEST_CASE("ChatSession set_permission_mode injects bypass", "[session][permission][45]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    // CLI --bypass-permissions 注入
    session->set_permission_mode(tool::PermissionMode::BypassPermissions);
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);

    // 注入后仍可两态循环（Bypass → Default → Bypass）
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);
}

// ============================================================
// 会话工作模式三态切换（标准 → 极简 → 计划 → 标准）+ 计划联动权限
// ============================================================

TEST_CASE("ChatSession session mode toggle cycles and links plan permission", "[session][mode]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    // 初始态：标准模式 + Default 权限
    REQUIRE(session->session_mode() == tool::SessionMode::Standard);
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);

    // 标准 → 极简：纯降级，权限不变
    session->toggle_session_mode();
    REQUIRE(session->session_mode() == tool::SessionMode::Minimal);
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);

    // 极简 → 计划：权限联动 Plan（保存 before_plan=Default）
    session->toggle_session_mode();
    REQUIRE(session->session_mode() == tool::SessionMode::Plan);
    REQUIRE(session->permission_mode() == tool::PermissionMode::Plan);

    // 计划模式下权限切换被忽略（由模式统一管理）
    session->toggle_permission_mode();
    REQUIRE(session->permission_mode() == tool::PermissionMode::Plan);

    // 计划 → 标准：退出计划，权限恢复 before_plan=Default
    session->toggle_session_mode();
    REQUIRE(session->session_mode() == tool::SessionMode::Standard);
    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);

    // 循环稳定：标准 → 极简 → 计划 再验证 before_plan 随 Bypass 权限保存/恢复
    session->set_permission_mode(tool::PermissionMode::BypassPermissions);
    session->toggle_session_mode();  // → Minimal，权限保持 Bypass
    REQUIRE(session->session_mode() == tool::SessionMode::Minimal);
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);
    session->toggle_session_mode();  // → Plan，权限联动 Plan
    REQUIRE(session->permission_mode() == tool::PermissionMode::Plan);
    session->toggle_session_mode();  // → Standard，恢复 Bypass
    REQUIRE(session->session_mode() == tool::SessionMode::Standard);
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);
}

TEST_CASE("ChatSession set_session_mode enters and exits plan", "[session][mode]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    session->set_permission_mode(tool::PermissionMode::BypassPermissions);
    session->set_session_mode(tool::SessionMode::Plan);
    REQUIRE(session->session_mode() == tool::SessionMode::Plan);
    REQUIRE(session->permission_mode() == tool::PermissionMode::Plan);

    // 退出计划：恢复进入前的权限（Bypass）
    session->set_session_mode(tool::SessionMode::Standard);
    REQUIRE(session->session_mode() == tool::SessionMode::Standard);
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);
}

// ============================================================
// 消息队列：模型忙碌时缓存用户输入（enqueue/remove/clear/merge）
// ============================================================

TEST_CASE("ChatSession queue rejects enqueue when idle", "[session][queue]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);
    REQUIRE_FALSE(session->is_generating());

    // 空闲时入队无意义：返回 false，调用方（TUI）应回退到直接发送路径
    REQUIRE_FALSE(session->enqueue_message("hello"));
    REQUIRE(session->queued_messages().empty());
}

TEST_CASE("ChatSession queue merges text with numbering", "[session][queue][merge]") {
    std::vector<QueuedMessageItem> items;
    QueuedMessageItem a;
    a.id = "a";
    a.text = "第一条";
    a.queued_at_ms = 1;
    QueuedMessageItem b;
    b.id = "b";
    b.text = "第二条";
    b.queued_at_ms = 2;
    items.push_back(a);
    items.push_back(b);

    const std::string merged = ChatSession::merge_queued_text(items);
    REQUIRE(merged.find("[排队消息 1/2]") != std::string::npos);
    REQUIRE(merged.find("第一条") != std::string::npos);
    REQUIRE(merged.find("[排队消息 2/2]") != std::string::npos);
    REQUIRE(merged.find("第二条") != std::string::npos);
    // 序号从 1 开始
    REQUIRE(merged.find("[排队消息 0/") == std::string::npos);
}

TEST_CASE("ChatSession queue enqueue/remove/clear while busy", "[session][queue]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    // 用阻塞 reader 挂起生成，制造"模型忙碌"窗口
    auto blocking = std::make_shared<BlockingStreamReader>();
    auto* provider = static_cast<MockCompletionProvider*>(session->completion_provider());
    provider->set_next_reader(blocking);

    session->send_message("hello");
    // 轮询等待 m_generating 置位（后台任务挂起在 next()）
    int tries = 0;
    while (!session->is_generating() && tries++ < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(session->is_generating());

    // 忙碌：入队成功（模型空闲时的 false 分支见前一用例）
    REQUIRE(session->enqueue_message("q2"));
    auto q1 = session->queued_messages();
    REQUIRE(q1.size() == 1);
    REQUIRE(q1[0].text == "q2");
    REQUIRE_FALSE(q1[0].id.empty());

    // 单条移除（✕ 按钮路径）
    session->remove_queued_message(q1[0].id);
    REQUIRE(session->queued_messages().empty());

    // 再入 2 条后整体清空（clear_history / new_session 路径）
    REQUIRE(session->enqueue_message("q3"));
    REQUIRE(session->enqueue_message("q4"));
    REQUIRE(session->queued_messages().size() == 2);
    session->clear_pending_queue();
    REQUIRE(session->queued_messages().empty());

    // 释放阻塞，让生成正常完成
    blocking->release();
    TaskManager::instance().waitForAll();
    REQUIRE_FALSE(session->is_generating());
}

TEST_CASE("ChatSession queue removed message cannot be removed twice", "[session][queue]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);

    // 空队列移除：无副作用（不崩溃、无新条目）
    session->remove_queued_message("nonexistent-id");
    REQUIRE(session->queued_messages().empty());
    session->clear_pending_queue();  // 空队列清空同样安全
    REQUIRE(session->queued_messages().empty());
}

TEST_CASE("ChatSession queue auto-sends after current loop ends", "[session][queue][autosend]") {
    MockConfigManager cfg;
    auto session = make_test_session(cfg);
    auto* provider = static_cast<MockCompletionProvider*>(session->completion_provider());

    // 第一轮：阻塞 reader 挂起生成，制造"模型忙碌"窗口
    auto blocking = std::make_shared<BlockingStreamReader>();
    provider->set_next_reader(blocking);
    // 第二轮（队列收尾冲刷）：正常 reader 产生回复
    auto turn2 = std::make_shared<MockStreamReader>();
    turn2->add_content_chunk("reply2");
    provider->set_next_reader(turn2);

    session->send_message("first");
    int tries = 0;
    while (!session->is_generating() && tries++ < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(session->is_generating());

    // 忙碌窗口内入队：期望当前循环结束后自动冲刷为新一轮
    REQUIRE(session->enqueue_message("queued-msg"));

    // 释放阻塞 → 第一轮完成 → flush_pending_after_run 应自动开启第二轮
    blocking->release();
    tries = 0;
    while (session->is_generating() && tries++ < 5000)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE_FALSE(session->is_generating());

    // 断言：整个会话消息里同时出现合并后的排队 user 消息与第二轮回复，
    // 证明排队消息确实被自动发送（而非停留在队列/丢失）
    const auto msgs = session->get_messages();
    REQUIRE(msgs.size() >= 4);  // user(first) + assistant + user(queued合并) + assistant(reply2)
    const auto& last = msgs.back();
    REQUIRE(last.role == ChatMessage::Role::Assistant);
    REQUIRE(last.content.find("reply2") != std::string::npos);
    bool found_queued_user = false;
    for (const auto& m : msgs) {
        if (m.role == ChatMessage::Role::User && m.content.find("queued-msg") != std::string::npos)
            found_queued_user = true;
    }
    REQUIRE(found_queued_user);
}

// ============================================================
// #87：PermissionMode / SessionMode 随 resume 持久化
// ============================================================
// 背景：修复前三态只存在于内存，resume 后一律重置为 Default —— 用户在 Plan 模式批准
// 方案后中断再恢复，agent 变成可写可执行的普通模式，而用户以为还在只读阶段。

namespace {

/// @brief 造一个可被 switch_session 恢复的会话文件（load_meta 需要 session_start）
/// @param permission 为空表示不写 permission 事件（模拟本特性之前的历史会话）
std::filesystem::path make_resumable_session(
    const std::string& file_name,
    const std::optional<agent::session::PermissionEvent>& permission) {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / file_name;
    fs::remove(tmp);

    auto store = std::make_shared<agent::session::SessionStore>(tmp.string(), file_name);
    if (!store->open()) throw std::runtime_error("open failed");
    store->append_session_start("/project", "model-x", "develop");
    store->append_user_message("u1", "", "hello", "t1");
    if (permission) store->append_permission(*permission);
    store->close();
    return tmp;
}

/// @brief 关闭会话持有的文件句柄后再删除临时文件
/// @details switch_session 以 append 模式重新打开会话文件，Windows 上未关闭的句柄
///          会让 remove 抛「文件被占用」（断言本身已通过，仅清理失败）。
void close_and_remove(std::unique_ptr<ChatSession>& session, const std::filesystem::path& p) {
    if (auto store = session->session_store()) store->close();
    session.reset();
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

}  // anonymous namespace

TEST_CASE("ChatSession switch_session restores plan permission boundary",
          "[session][permission][87]") {
    MockConfigManager cfg;

    agent::session::PermissionEvent ev;
    ev.permission_mode = "plan";
    ev.session_mode = "plan";
    ev.permission_mode_before_plan = "bypass-permissions";
    ev.in_plan = true;
    auto tmp = make_resumable_session("workx_test_perm_restore.jsonl", ev);

    auto session = make_test_session(cfg);
    REQUIRE(session->switch_session(tmp.string()));

    // 核心断言：Plan 只读边界在 resume 后仍然成立（修复前这里会是 Default）
    REQUIRE(session->permission_mode() == tool::PermissionMode::Plan);
    REQUIRE(session->session_mode() == tool::SessionMode::Plan);

    const auto r = session->last_permission_restore();
    REQUIRE(r.restored == true);
    REQUIRE(r.reason.empty());  // 成功恢复不提示用户
    REQUIRE(r.mode == tool::PermissionMode::Plan);

    close_and_remove(session, tmp);
}

TEST_CASE("ChatSession switch_session falls back to default when no permission record",
          "[session][permission][87]") {
    MockConfigManager cfg;

    // 历史会话：没有任何 permission 事件
    auto tmp = make_resumable_session("workx_test_perm_no_record.jsonl", std::nullopt);

    auto session = make_test_session(cfg);
    REQUIRE(session->switch_session(tmp.string()));

    REQUIRE(session->permission_mode() == tool::PermissionMode::Default);
    REQUIRE(session->session_mode() == tool::SessionMode::Standard);

    // 回退必须显式告知（reason 非空 → TUI 插一行提示），不能静默降级
    const auto r = session->last_permission_restore();
    REQUIRE(r.restored == false);
    REQUIRE(r.reason == "no_record");

    close_and_remove(session, tmp);
}

TEST_CASE("ChatSession switch_session falls back to plan on invalid permission value",
          "[session][permission][87]") {
    MockConfigManager cfg;

    agent::session::PermissionEvent ev;
    ev.permission_mode = "bogus-mode";  // 跨版本/被篡改
    ev.session_mode = "standard";
    auto tmp = make_resumable_session("workx_test_perm_invalid.jsonl", ev);

    auto session = make_test_session(cfg);
    REQUIRE(session->switch_session(tmp.string()));

    // 值非法：从严回退 Plan（宁可只读，不可放行写操作）
    REQUIRE(session->permission_mode() == tool::PermissionMode::Plan);
    REQUIRE(session->session_mode() == tool::SessionMode::Plan);

    const auto r = session->last_permission_restore();
    REQUIRE(r.restored == false);
    REQUIRE(r.reason == "invalid_value");

    close_and_remove(session, tmp);
}

TEST_CASE("ChatSession persists permission change after resume", "[session][permission][87]") {
    MockConfigManager cfg;

    auto tmp = make_resumable_session("workx_test_perm_persist.jsonl", std::nullopt);
    auto session = make_test_session(cfg);
    REQUIRE(session->switch_session(tmp.string()));

    // switch_session 后会话文件以 append 模式重新打开 → 后续模式变更应落盘
    REQUIRE(session->session_store() != nullptr);

    session->set_permission_mode(tool::PermissionMode::BypassPermissions);
    REQUIRE(session->permission_mode() == tool::PermissionMode::BypassPermissions);

    session->session_store()->close();
    auto latest = agent::session::SessionStore::load_permission(tmp.string());
    REQUIRE(latest.has_value());
    REQUIRE(latest->permission_mode == "bypass-permissions");

    close_and_remove(session, tmp);
}
