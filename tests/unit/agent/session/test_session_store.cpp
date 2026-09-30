/**
 * @file test_session_store.cpp
 * @brief JSONL SessionStore 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <chrono>

#include "agent/session/session_store.h"
#include "agent/tool/TodoStore/todo_store.h"
#include "core/todo/todo_item.h"

using namespace agent::session;

namespace {

/// @brief 临时文件 RAII 清理
class TempFile {
   public:
    explicit TempFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove(path_);
    }
    ~TempFile() { std::filesystem::remove(path_); }
    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }

   private:
    std::filesystem::path path_;
};

class TempDir {
   public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }

   private:
    std::filesystem::path path_;
};

/// @brief 与 session_store.cpp 内部 djb2_hex 保持一致的稳定 hash 计算（测试断言用）
std::string djb2_for_test(const std::string& s) {
    unsigned long h = 5381;
    for (unsigned char c : s) h = h * 33 + c;
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << h;
    return oss.str();
}

}  // anonymous namespace

// ============================================================
// 追加与读取
// ============================================================

TEST_CASE("session_store: append and read back", "[session][store]") {
    TempFile tmp("workx_test_session.jsonl");

    SessionStore store(tmp.string(), "test-session-id");
    REQUIRE(store.open());

    REQUIRE(store.append_session_start("/cwd", "test-model", "main"));
    REQUIRE(store.append_user_message("u1", "", "hello", "2026-07-31T10:00:00Z"));
    REQUIRE(store.append_assistant_message("a1", "u1", "hi there", "", {}, "2026-07-31T10:00:01Z"));
    REQUIRE(store.append_session_end());

    store.close();

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 4);
    REQUIRE(events[0]["type"] == "session_start");
    REQUIRE(events[0]["cwd"] == "/cwd");
    REQUIRE(events[0]["sessionId"] == "test-session-id");
    REQUIRE(events[1]["type"] == "user");
    REQUIRE(events[1]["content"] == "hello");
    REQUIRE(events[2]["type"] == "assistant");
    REQUIRE(events[2]["content"] == "hi there");
    REQUIRE(events[2]["parentUuid"] == "u1");
    REQUIRE(events[3]["type"] == "session_end");
    REQUIRE(events[3]["sessionId"] == "test-session-id");
}

TEST_CASE("session_store: append is idempotent (no truncate)", "[session][store]") {
    // 验证重复 open 不清空文件
    TempFile tmp("workx_test_append.jsonl");

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(store.append_user_message("u1", "", "first", "t1"));
        store.close();
    }

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(store.append_user_message("u2", "", "second", "t2"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);
    REQUIRE(events[0]["content"] == "first");
    REQUIRE(events[1]["content"] == "second");
}

// ============================================================
// system_prompt 事件
// ============================================================

TEST_CASE("session_store: system_prompt records reason/content/hash", "[session][store]") {
    TempFile tmp("workx_test_sysprompt.jsonl");

    {
        SessionStore store(tmp.string(), "test-session-id");
        REQUIRE(store.open());
        REQUIRE(store.append_system_prompt("initial", "you are a helpful assistant"));
        REQUIRE(store.append_system_prompt("changed", "you are a strict reviewer"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);
    REQUIRE(events[0]["type"] == "system_prompt");
    REQUIRE(events[0]["reason"] == "initial");
    REQUIRE(events[0]["content"] == "you are a helpful assistant");
    REQUIRE(events[0]["sessionId"] == "test-session-id");
    REQUIRE_FALSE(events[0]["hash"].get<std::string>().empty());
    REQUIRE(events[1]["reason"] == "changed");
    // 内容不同 → hash 必须不同（前端据此检测提示词变更）
    REQUIRE(events[0]["hash"].get<std::string>() != events[1]["hash"].get<std::string>());
    // hash 稳定：相同内容两次写入得到相同 hash
    REQUIRE(events[0]["hash"].get<std::string>() == djb2_for_test("you are a helpful assistant"));
}

// ============================================================
// assistant 消息含 tool_uses
// ============================================================

TEST_CASE("session_store: assistant with tool_uses round-trip", "[session][store]") {
    TempFile tmp("workx_test_tooluses.jsonl");

    std::vector<agent::ToolUse> uses;
    uses.push_back({"toolu_001", "Read", nlohmann::json({{"path", "/tmp/test"}})});
    uses.push_back(
        {"toolu_002", "Write", nlohmann::json({{"path", "/tmp/out"}, {"content", "hi"}})});

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(
            store.append_assistant_message("a1", "u1", "calling tools", "thinking...", uses, "t1"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 1);
    REQUIRE(events[0]["toolUses"].size() == 2);
    REQUIRE(events[0]["toolUses"][0]["id"] == "toolu_001");
    REQUIRE(events[0]["toolUses"][0]["name"] == "Read");
    REQUIRE(events[0]["toolUses"][0]["input"]["path"] == "/tmp/test");
    REQUIRE(events[0]["toolUses"][1]["name"] == "Write");
}

// ============================================================
// assistant 消息思考时长（reasoningMs）
// ============================================================

TEST_CASE("session_store: assistant reasoningMs round-trip", "[session][store]") {
    TempFile tmp("workx_test_reasoningms.jsonl");

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(store.append_assistant_message("a1", "u1", "answer", "thinking", {}, "t2", 4200.0));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 1);
    REQUIRE(events[0]["reasoningMs"] == 4200.0);

    auto messages = SessionStore::load_messages(tmp.string());
    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0].reasoning_ms == 4200.0);
}

// ============================================================
// list_sessions
// ============================================================

TEST_CASE("session_store: list sessions in project dir", "[session][store]") {
    TempDir tmp("workx_test_project");

    auto f1 = tmp.path() / "aaa-111.jsonl";
    auto f2 = tmp.path() / "bbb-222.jsonl";
    {
        std::ofstream(f1.string())
            << R"({"type":"session_start","sessionId":"aaa-111","createdAt":"2026-07-30T10:00:00Z"})"
            << "\n";
    }
    // 确保两个文件的修改时间可区分（Windows 文件系统精度可能较低）
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream(f2.string())
            << R"({"type":"session_start","sessionId":"bbb-222","createdAt":"2026-07-31T10:00:00Z"})"
            << "\n";
    }

    auto sessions = SessionStore::list_sessions(tmp.string());
    REQUIRE(sessions.size() == 2);
    // 按修改时间倒序（f2 后创建，应在前）
    REQUIRE(sessions[0].session_id == "bbb-222");
    REQUIRE(sessions[1].session_id == "aaa-111");
}

TEST_CASE("session_store: list sessions empty dir", "[session][store]") {
    TempDir tmp("workx_test_empty_project");
    auto sessions = SessionStore::list_sessions(tmp.string());
    REQUIRE(sessions.empty());
}

TEST_CASE("session_store: list sessions non-existent dir", "[session][store]") {
    auto sessions = SessionStore::list_sessions("/nonexistent/path/12345");
    REQUIRE(sessions.empty());
}

// ============================================================
// load_messages
// ============================================================

TEST_CASE("session_store: load messages from jsonl", "[session][store]") {
    TempFile tmp("workx_test_load.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/tmp","model":"m","gitBranch":"main"})"
          << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hello"})"
          << "\n";
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","timestamp":"t2","content":"hi","reasoningContent":"thinking","toolUses":[]})"
          << "\n";
        f << R"({"type":"tool","uuid":"t1","parentUuid":"a1","timestamp":"t3","toolCallId":"tc1","toolName":"Read","content":"file content","isError":false})"
          << "\n";
        f << R"({"type":"session_end","sessionId":"s1"})" << "\n";
    }

    auto messages = SessionStore::load_messages(tmp.string());
    REQUIRE(messages.size() == 3);  // user + assistant + tool（不含 session_start/end）
    REQUIRE(messages[0].role == agent::ChatMessage::Role::User);
    REQUIRE(messages[0].content == "hello");
    REQUIRE(messages[1].role == agent::ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "hi");
    REQUIRE(messages[1].reasoning_content == "thinking");
    REQUIRE(messages[2].role == agent::ChatMessage::Role::Tool);
    REQUIRE(messages[2].tool_call_id == "tc1");
    REQUIRE(messages[2].tool_name == "Read");
    REQUIRE(messages[2].content == "file content");
    REQUIRE_FALSE(messages[2].is_error);
}

TEST_CASE("session_store: load messages with tool_uses preserved", "[session][store]") {
    TempFile tmp("workx_test_load_uses.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"","timestamp":"t1","content":"call","reasoningContent":"","toolUses":[{"id":"toolu_1","name":"Read","input":{"path":"/tmp"}}]})"
          << "\n";
    }

    auto messages = SessionStore::load_messages(tmp.string());
    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0].tool_uses.size() == 1);
    REQUIRE(messages[0].tool_uses[0].id == "toolu_1");
    REQUIRE(messages[0].tool_uses[0].name == "Read");
    REQUIRE(messages[0].tool_uses[0].input["path"] == "/tmp");
}

// ============================================================
// load_meta
// ============================================================

TEST_CASE("session_store: load meta from jsonl", "[session][store]") {
    TempFile tmp("workx_test_meta.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"meta-1","cwd":"/project","model":"gpt-4","gitBranch":"develop"})"
          << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hi"})"
          << "\n";
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","timestamp":"t2","content":"","reasoningContent":"","toolUses":[]})"
          << "\n";
        f << R"({"type":"tool","uuid":"t1","parentUuid":"a1","timestamp":"t3","toolCallId":"c1","toolName":"Read","content":"","isError":false})"
          << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->session_id == "meta-1");
    REQUIRE(meta->cwd == "/project");
    REQUIRE(meta->model == "gpt-4");
    REQUIRE(meta->git_branch == "develop");
    REQUIRE(meta->message_count == 3);  // user + assistant + tool
}

TEST_CASE("session_store: load meta fallback to filename", "[session][store]") {
    // 没有 session_start 事件时，用文件名作为 session_id
    TempFile tmp("fallback-id.jsonl");
    {
        std::ofstream f(tmp.string());
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hi"})"
          << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->session_id == "fallback-id");
    REQUIRE(meta->message_count == 1);
}

// ============================================================
// 损坏行容错
// ============================================================

TEST_CASE("session_store: skip corrupted lines", "[session][store]") {
    TempFile tmp("workx_test_corrupt.jsonl");
    {
        std::ofstream f(tmp.string());
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"good1"})"
          << "\n";
        f << "this is not json\n";
        f << R"({"type":"user","uuid":"u2","parentUuid":"","timestamp":"t2","content":"good2"})"
          << "\n";
        f << "{broken json\n";
        f << R"({"type":"user","uuid":"u3","parentUuid":"","timestamp":"t3","content":"good3"})"
          << "\n";
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 3);  // 跳过 2 行损坏
    REQUIRE(events[0]["content"] == "good1");
    REQUIRE(events[1]["content"] == "good2");
    REQUIRE(events[2]["content"] == "good3");
}

// ============================================================
// get_project_session_dir
// ============================================================

TEST_CASE("session_store: get_project_session_dir", "[session][store]") {
    auto dir = get_project_session_dir("C:/Users/test/.workx", R"(D:\develop\workx)");
    std::string s = dir.string();
    // 应包含 projects 和编码后的路径
    REQUIRE(s.find("projects") != std::string::npos);
    REQUIRE(s.find("D--develop-workx") != std::string::npos);
}

// ============================================================
// title 事件
// ============================================================

TEST_CASE("session_store: append_title writes title event", "[session][store][title]") {
    TempFile tmp("workx_test_title_append.jsonl");

    {
        SessionStore store(tmp.string(), "title-session-1");
        REQUIRE(store.open());
        REQUIRE(store.append_session_start("/cwd", "model", "branch"));
        REQUIRE(store.append_title("我的会话标题"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);
    REQUIRE(events[1]["type"] == "title");
    REQUIRE(events[1]["title"] == "我的会话标题");
    REQUIRE(events[1]["sessionId"] == "title-session-1");
}

TEST_CASE("session_store: load_meta reads title from last title event", "[session][store][title]") {
    TempFile tmp("workx_test_title_meta.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/p","model":"m","gitBranch":"b","createdAt":"2026-07-31T10:00:00Z"})"
          << "\n";
        f << R"({"type":"title","sessionId":"s1","timestamp":"t1","title":"旧标题"})" << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t2","content":"hello"})"
          << "\n";
        f << R"({"type":"title","sessionId":"s1","timestamp":"t3","title":"新标题"})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "新标题");  // 取最后一条 title 事件
}

TEST_CASE("session_store: load_meta title fallback to first user message 20 chars",
          "[session][store][title]") {
    TempFile tmp("workx_test_title_fallback.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/p","model":"m","gitBranch":"b","createdAt":"2026-07-31T10:00:00Z"})"
          << "\n";
        // 无 title 事件，应 fallback 到首条 user 消息前 20 字
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"这是一个比较长的用户消息用于测试标题截断功能是否正常工作"})"
          << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    // 前 20 字 + "..."
    REQUIRE(meta->title.size() > 20);
    REQUIRE(meta->title.find("这是一个比较长的用户消息用于测试标题截断") != std::string::npos);
    REQUIRE(meta->title.back() == '.');  // 以 "..." 结尾
}

TEST_CASE("session_store: load_meta title fallback short message no ellipsis",
          "[session][store][title]") {
    TempFile tmp("workx_test_title_short.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"短消息"})"
          << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "短消息");  // 不足 20 字，不加 "..."
}

TEST_CASE("session_store: load_meta title fallback no user message", "[session][store][title]") {
    TempFile tmp("workx_test_title_empty.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/p","model":"m","gitBranch":"b","createdAt":"2026-07-31T10:00:00Z"})"
          << "\n";
        // 无 title 事件，无 user 消息
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "未命名会话");
}

TEST_CASE("session_store: title fallback UTF-8 safe truncation", "[session][store][title]") {
    TempFile tmp("workx_test_title_utf8.jsonl");

    {
        std::ofstream f(tmp.string());
        // 中文每字 3 字节 UTF-8，20 字 = 60 字节
        // 用 25 个中文字 = 75 字节，应截断到 20 字（60 字节）+ "..."
        std::string content(25, '\xE4');  // 无效 UTF-8，仅测试不崩溃
        // 改用真实中文字符
        content = "一二三四五六七八九十一二三四五六七八九十一二三四五";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":")" << content
          << R"("})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    // 应截断到前 20 字 + "..."，不应截断多字节字符
    REQUIRE(meta->title.find("...") != std::string::npos);
}

TEST_CASE("session_store: multiple title events override", "[session][store][title]") {
    TempFile tmp("workx_test_title_multi.jsonl");

    {
        SessionStore store(tmp.string(), "multi-title");
        REQUIRE(store.open());
        store.append_session_start("/p", "m", "b");
        store.append_title("第一版");
        store.append_title("第二版");
        store.append_title("第三版");
        store.close();
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "第三版");  // 最后一条生效
}

// ============================================================
// #24：Todo 清单全链路往返（TodoStore → JSONL → load_todos → restore_todos）
// ============================================================

TEST_CASE("session_store: todo persist restore roundtrip via TodoStore", "[session][store][todo]") {
    auto& store = agent::tool::TodoStore::instance();
    store.clear_for_test();

    TempFile tmp("workx_test_todo_roundtrip.jsonl");
    const std::string session_id = "todo-session-1";

    // 1. 持久化：接线 persist_cb（同 ChatSession::wire_todo_persistence）→
    //    TodoStore 每次变更写 JSONL todo 快照
    {
        SessionStore s(tmp.string(), session_id);
        REQUIRE(s.open());
        store.set_persist_callback(
            session_id,
            [&s](const std::vector<core::todo::TodoItem>& todos) { s.append_todo(todos); });

        core::todo::TodoItem a;
        a.content = "探索代码";
        a.active_form = "探索代码中";
        a.status = core::todo::TodoStatus::InProgress;
        REQUIRE(store.create_todo(session_id, a) == "1");  // 自增 id

        core::todo::TodoItem b;
        b.content = "跑测试";
        REQUIRE(store.create_todo(session_id, b) == "2");
        s.close();
    }

    // 2. 读回：load_todos 取最后一条 todo 快照（append-only 覆盖语义）
    auto todos = SessionStore::load_todos(tmp.string());
    REQUIRE(todos.size() == 2);
    REQUIRE(todos[0].id == "1");
    REQUIRE(todos[0].content == "探索代码");
    REQUIRE(todos[0].active_form == "探索代码中");
    REQUIRE(todos[0].status == core::todo::TodoStatus::InProgress);
    REQUIRE(todos[1].id == "2");
    REQUIRE(todos[1].content == "跑测试");
    REQUIRE(todos[1].status == core::todo::TodoStatus::Pending);

    // 3. 恢复：restore_todos 复原内存态 + next_id 高水位（同 switch_session）。
    //    真实场景中 switch_session 先 restore（新 session_id 的 persist_cb 尚为空），
    //    再 wire_todo_persistence 接线。此处先解除回调，避免恢复快照触发
    //    持久化回调写回已关闭的 store（悬垂引用）。
    store.set_persist_callback(session_id, {});
    store.restore_todos(session_id, todos);
    auto restored = store.list_todos(session_id);
    REQUIRE(restored.size() == 2);
    REQUIRE(restored[0].content == "探索代码");
    REQUIRE(restored[1].content == "跑测试");

    // next_id 高水位：恢复后新建应继续分配 3（不复用旧 id）
    core::todo::TodoItem c;
    c.content = "第三项";
    REQUIRE(store.create_todo(session_id, c) == "3");
    REQUIRE(store.list_todos(session_id).size() == 3);

    store.clear_for_test();
}

TEST_CASE("session_store: todo empty snapshot clears on resume", "[session][store][todo]") {
    auto& store = agent::tool::TodoStore::instance();
    store.clear_for_test();

    TempFile tmp("workx_test_todo_empty.jsonl");
    const std::string session_id = "todo-session-2";

    // 写入两个 todo 快照，最后追加空快照（清空语义：reset_session / 全部完成）
    {
        SessionStore s(tmp.string(), session_id);
        REQUIRE(s.open());
        store.set_persist_callback(
            session_id,
            [&s](const std::vector<core::todo::TodoItem>& todos) { s.append_todo(todos); });

        core::todo::TodoItem a;
        a.content = "会消失的任务";
        store.create_todo(session_id, a);
        s.append_todo({});  // 空快照：模拟 reset_session 清空落盘
        s.close();
    }

    // 恢复应得到空清单（最后一条快照为空 → 不残留旧任务）
    // 先解除回调（同上：避免 restore 触发写回已析构的 store）
    store.set_persist_callback(session_id, {});
    auto todos = SessionStore::load_todos(tmp.string());
    REQUIRE(todos.empty());
    store.restore_todos(session_id, todos);
    REQUIRE(store.list_todos(session_id).empty());

    store.clear_for_test();
}

// ============================================================
// #87：permission 事件（权限模式持久化）
// ============================================================

TEST_CASE("session_store: permission event round-trip", "[session][store][permission]") {
    TempFile tmp("workx_test_permission.jsonl");

    {
        SessionStore store(tmp.string(), "perm-session");
        REQUIRE(store.open());

        PermissionEvent ev;
        ev.permission_mode = "plan";
        ev.session_mode = "plan";
        ev.permission_mode_before_plan = "default";
        ev.in_plan = true;
        REQUIRE(store.append_permission(ev));
        store.close();
    }

    auto loaded = SessionStore::load_permission(tmp.string());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->permission_mode == "plan");
    REQUIRE(loaded->session_mode == "plan");
    REQUIRE(loaded->permission_mode_before_plan == "default");
    REQUIRE(loaded->in_plan == true);
}

TEST_CASE("session_store: load_permission takes the last event", "[session][store][permission]") {
    TempFile tmp("workx_test_permission_last.jsonl");

    {
        SessionStore store(tmp.string(), "perm-session-2");
        REQUIRE(store.open());

        // 模拟一次真实会话：默认 → 用户 Shift+Tab 切 bypass → agent 进入 plan
        PermissionEvent first;
        first.permission_mode = "default";
        first.session_mode = "standard";
        REQUIRE(store.append_permission(first));

        PermissionEvent second;
        second.permission_mode = "bypass-permissions";
        second.session_mode = "standard";
        REQUIRE(store.append_permission(second));

        PermissionEvent third;
        third.permission_mode = "plan";
        third.session_mode = "plan";
        third.permission_mode_before_plan = "bypass-permissions";
        third.in_plan = true;
        REQUIRE(store.append_permission(third));
        store.close();
    }

    // append-only 语义：恢复的是「最后一次处于什么模式」，不是初始模式
    auto loaded = SessionStore::load_permission(tmp.string());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->permission_mode == "plan");
    REQUIRE(loaded->permission_mode_before_plan == "bypass-permissions");
}

TEST_CASE("session_store: load_permission on file without permission event",
          "[session][store][permission]") {
    TempFile tmp("workx_test_permission_none.jsonl");

    {
        SessionStore store(tmp.string(), "perm-session-3");
        REQUIRE(store.open());
        REQUIRE(store.append_session_start("/cwd", "model", "main"));
        store.close();
    }

    // 历史会话（本特性之前创建）没有 permission 事件 → nullopt，由调用方走 fail-safe
    REQUIRE_FALSE(SessionStore::load_permission(tmp.string()).has_value());
}

TEST_CASE("session_store: load_permission ignores malformed event",
          "[session][store][permission]") {
    TempFile tmp("workx_test_permission_bad.jsonl");

    {
        std::ofstream ofs(tmp.path(), std::ios::app);
        ofs << "{\"type\":\"permission\"}\n";  // 缺全部字段
        ofs << "not a json line\n";            // 坏行：不应影响后续解析
        ofs << "{\"type\":\"permission\",\"permissionMode\":\"bogus-mode\","
               "\"sessionMode\":\"standard\"}\n";
    }

    // 坏值照样读出（值合法性由 ChatSession 判定 → 走 invalid_value 从严分支）
    auto loaded = SessionStore::load_permission(tmp.string());
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->permission_mode == "bogus-mode");
}
