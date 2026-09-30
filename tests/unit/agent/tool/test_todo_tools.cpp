/**
 * @file test_todo_tools.cpp
 * @brief #24：TodoStore / TodoWriteTool / TaskV2 系列（Create/Get/Update/List）
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "agent/tool/TodoStore/todo_store.h"
#include "agent/tool/TodoWriteTool/todo_write_tool.h"
#include "agent/tool/TaskTools/task_create_tool.h"
#include "agent/tool/TaskTools/task_get_tool.h"
#include "agent/tool/TaskTools/task_update_tool.h"
#include "agent/tool/TaskTools/task_list_tool.h"
#include "agent/tool/itool.h"
#include "agent/session/session_store.h"
#include "core/events/agent_events.h"
#include "core/todo/todo_item.h"

#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::test;

namespace {

/// @brief 待办工具测试夹具：清理 TodoStore 单例残留
struct TodoFixture {
    TodoFixture() { TodoStore::instance().clear_for_test(); }
    ~TodoFixture() { TodoStore::instance().clear_for_test(); }
};

/// @brief 填充最小 ToolContext（待办工具仅需 session_id）
void fill_ctx(ToolContext& ctx, const std::string& session = "test-session") {
    ctx.cwd = ".";
    ctx.session_id = session;
}

core::todo::TodoItem make_item(const std::string& content,
                               core::todo::TodoStatus status = core::todo::TodoStatus::Pending) {
    core::todo::TodoItem item;
    item.content = content;
    item.active_form = content + "ing";
    item.status = status;
    return item;
}

}  // namespace

// ============================================================
// TodoStore CRUD
// ============================================================

TEST_CASE_METHOD(TodoFixture, "TodoStore create assigns auto-increment ids", "[todo][store]") {
    auto& store = TodoStore::instance();
    auto a = store.create_todo("s1", make_item("Run tests"));
    auto b = store.create_todo("s1", make_item("Write docs"));
    REQUIRE(a == "1");
    REQUIRE(b == "2");

    auto todos = store.list_todos("s1");
    REQUIRE(todos.size() == 2);
    REQUIRE(todos[0].id == "1");
    REQUIRE(todos[1].id == "2");
}

TEST_CASE_METHOD(TodoFixture, "TodoStore get/update/delete", "[todo][store]") {
    auto& store = TodoStore::instance();
    store.create_todo("s1", make_item("Task A"));

    // get
    auto got = store.get_todo("s1", "1");
    REQUIRE(got.has_value());
    REQUIRE(got->content == "Task A");

    // update
    bool updated = store.update_todo("s1", "1", [](core::todo::TodoItem& t) {
        t.status = core::todo::TodoStatus::Completed;
        t.content = "Task A done";
    });
    REQUIRE(updated);
    got = store.get_todo("s1", "1");
    REQUIRE(got->status == core::todo::TodoStatus::Completed);
    REQUIRE(got->content == "Task A done");

    // update missing id → false
    REQUIRE_FALSE(store.update_todo("s1", "999", [](core::todo::TodoItem&) {}));

    // delete
    REQUIRE(store.delete_todo("s1", "1"));
    REQUIRE_FALSE(store.get_todo("s1", "1").has_value());
    REQUIRE_FALSE(store.delete_todo("s1", "1"));  // 已删除
}

TEST_CASE_METHOD(TodoFixture, "TodoStore sessions are isolated", "[todo][store]") {
    auto& store = TodoStore::instance();
    store.create_todo("s1", make_item("A"));
    store.create_todo("s2", make_item("B"));
    REQUIRE(store.list_todos("s1").size() == 1);
    REQUIRE(store.list_todos("s2").size() == 1);
    REQUIRE(store.list_todos("s3").empty());
}

TEST_CASE_METHOD(TodoFixture, "TodoStore replace_todos all-completed clears list",
                 "[todo][store]") {
    auto& store = TodoStore::instance();
    store.create_todo("s1", make_item("A"));

    // 全部 completed → 置空（对齐 cc allDone ? [] : todos）
    std::vector<core::todo::TodoItem> done = {
        make_item("A", core::todo::TodoStatus::Completed),
        make_item("B", core::todo::TodoStatus::Completed),
    };
    auto result = store.replace_todos("s1", done);
    REQUIRE(result.empty());
    REQUIRE(store.list_todos("s1").empty());
}

TEST_CASE_METHOD(TodoFixture, "TodoStore publishes TodoUpdatedEvent on change",
                 "[todo][store][event]") {
    MockEventBus bus;
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);  // publish_async 立即派发（模拟宿主即时响应）
    std::vector<TodoUpdatedEvent> events;
    bus.subscribe<TodoUpdatedEvent>([&events](const TodoUpdatedEvent& e) { events.push_back(e); });
    TodoStore::instance().set_event_bus(&bus);

    TodoStore::instance().create_todo("s1", make_item("A"));
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].session_id == "s1");
    REQUIRE(events[0].todos.size() == 1);
    REQUIRE(events[0].todos[0].content == "A");
}

TEST_CASE_METHOD(TodoFixture, "TodoStore restore_todos resumes ids above max", "[todo][store]") {
    auto& store = TodoStore::instance();
    std::vector<core::todo::TodoItem> restored = {
        make_item("A"),
        make_item("B"),
    };
    restored[0].id = "3";
    restored[1].id = "7";
    store.restore_todos("s1", restored);

    // 恢复后 next_id 应超过最大 id（8），避免 id 复用
    auto new_id = store.create_todo("s1", make_item("C"));
    REQUIRE(new_id == "8");
}

TEST_CASE_METHOD(TodoFixture,
                 "TodoStore reset_session clears todos, writes empty snapshot, keeps persist_cb",
                 "[todo][store]") {
    auto& store = TodoStore::instance();
    store.create_todo("s1", make_item("A"));

    // 注册持久化回调（模拟 ChatSession wire_todo_persistence）
    std::vector<std::vector<core::todo::TodoItem>> persisted;
    store.set_persist_callback(
        "s1", [&](const std::vector<core::todo::TodoItem>& todos) { persisted.push_back(todos); });

    store.reset_session("s1");
    REQUIRE(store.list_todos("s1").empty());

    // 空快照已写入 JSONL（/resume 不再恢复旧清单）
    REQUIRE(persisted.size() == 1);
    REQUIRE(persisted[0].empty());

    // next_id 重置：新任务从 1 开始
    auto new_id = store.create_todo("s1", make_item("B"));
    REQUIRE(new_id == "1");

    // 持久化回调仍绑定：清空后新变更继续持久化
    REQUIRE(persisted.size() == 2);
    REQUIRE(persisted[1].size() == 1);
    REQUIRE(persisted[1][0].content == "B");
}

// ============================================================
// TodoWriteTool（全量替换）
// ============================================================

TEST_CASE_METHOD(TodoFixture, "TodoWriteTool replaces full list", "[todo][todowrite]") {
    TodoWriteTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    nlohmann::json input = {
        {"todos",
         nlohmann::json::array({
             {{"content", "Run tests"}, {"status", "in_progress"}, {"activeForm", "Running tests"}},
             {{"content", "Write docs"}, {"status", "pending"}, {"activeForm", "Writing docs"}},
         })}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().data["newTodos"].size() == 2);
    REQUIRE(r.value().data["newTodos"][0]["content"] == "Run tests");
    REQUIRE(r.value().data["newTodos"][0]["status"] == "in_progress");
}

TEST_CASE_METHOD(TodoFixture, "TodoWriteTool rejects missing todos array", "[todo][todowrite]") {
    TodoWriteTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json::object(), ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

TEST_CASE_METHOD(TodoFixture, "TodoWriteTool rejects empty content", "[todo][todowrite]") {
    TodoWriteTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    nlohmann::json input = {
        {"todos", nlohmann::json::array({
                      {{"content", ""}, {"status", "pending"}, {"activeForm", ""}},
                  })}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
}

TEST_CASE_METHOD(TodoFixture, "TodoWriteTool rejects wrong-typed status", "[todo][todowrite]") {
    TodoWriteTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    nlohmann::json input = {
        {"todos", nlohmann::json::array({
                      {{"content", "Run tests"}, {"status", 42}, {"activeForm", "Running tests"}},
                  })}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
    // 校验失败不产生任何写入
    REQUIRE(TodoStore::instance().list_todos("test-session").empty());
}

TEST_CASE_METHOD(TodoFixture, "TodoWriteTool preserves ids for matching content",
                 "[todo][todowrite]") {
    // 先用 TaskCreate 创建带 id 的任务
    TaskCreateTool create_tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto c1 = create_tool.call(nlohmann::json{{"subject", "Run tests"}}, ctx);
    auto c2 = create_tool.call(nlohmann::json{{"subject", "Write docs"}}, ctx);
    REQUIRE(c1.is_ok());
    REQUIRE(c2.is_ok());
    REQUIRE(c1.value().data["task"]["id"] == "1");
    REQUIRE(c2.value().data["task"]["id"] == "2");

    // TodoWrite 全量替换：content 匹配的条目应继承原 id（与顺序无关）
    TodoWriteTool write_tool;
    nlohmann::json input = {
        {"todos",
         nlohmann::json::array({
             {{"content", "Write docs"}, {"status", "in_progress"}, {"activeForm", "Writing docs"}},
             {{"content", "Run tests"}, {"status", "completed"}, {"activeForm", "Running tests"}},
         })}};
    auto r = write_tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().data["newTodos"].size() == 2);

    auto& store = TodoStore::instance();
    auto todos = store.list_todos("test-session");
    REQUIRE(todos.size() == 2);
    auto it_docs = std::find_if(todos.begin(), todos.end(), [](const core::todo::TodoItem& t) {
        return t.content == "Write docs";
    });
    auto it_tests = std::find_if(todos.begin(), todos.end(), [](const core::todo::TodoItem& t) {
        return t.content == "Run tests";
    });
    REQUIRE(it_docs != todos.end());
    REQUIRE(it_tests != todos.end());
    REQUIRE(it_docs->id == "2");
    REQUIRE(it_tests->id == "1");

    // 后续 TaskUpdate 按 id 仍可操作（id 未断裂）
    TaskUpdateTool update_tool;
    auto u = update_tool.call(nlohmann::json{{"taskId", "2"}, {"status", "completed"}}, ctx);
    REQUIRE(u.is_ok());
    REQUIRE(u.value().data["task"]["status"] == "completed");
}

// ============================================================
// TaskV2 系列
// ============================================================

TEST_CASE_METHOD(TodoFixture, "TaskCreateTool creates task and returns id", "[todo][task]") {
    TaskCreateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto r = tool.call(
        nlohmann::json{
            {"subject", "Refactor module"},
            {"description", "Split into smaller files"},
            {"activeForm", "Refactoring module"},
        },
        ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().data["task"]["id"] == "1");
    REQUIRE(r.value().data["task"]["subject"] == "Refactor module");

    auto got = TodoStore::instance().get_todo("test-session", "1");
    REQUIRE(got.has_value());
    REQUIRE(got->description == "Split into smaller files");
}

TEST_CASE_METHOD(TodoFixture, "TaskCreateTool rejects missing subject", "[todo][task]") {
    TaskCreateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json{{"description", "no subject"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

TEST_CASE_METHOD(TodoFixture, "TaskCreateTool rejects wrong-typed subject", "[todo][task]") {
    TaskCreateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json{{"subject", 123}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
    // 校验失败不产生任何写入
    REQUIRE(TodoStore::instance().list_todos("test-session").empty());
}

TEST_CASE_METHOD(TodoFixture, "TaskGetTool returns task or null", "[todo][task]") {
    TodoStore::instance().create_todo("test-session", make_item("Existing"));

    TaskGetTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto ok = tool.call(nlohmann::json{{"taskId", "1"}}, ctx);
    REQUIRE(ok.is_ok());
    REQUIRE(ok.value().data["task"]["content"] == "Existing");

    // 不存在 → null（非错误）
    auto missing = tool.call(nlohmann::json{{"taskId", "999"}}, ctx);
    REQUIRE(missing.is_ok());
    REQUIRE(missing.value().data["task"].is_null());
}

TEST_CASE_METHOD(TodoFixture, "TaskGetTool rejects missing taskId", "[todo][task]") {
    TaskGetTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json::object(), ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

TEST_CASE_METHOD(TodoFixture, "TaskUpdateTool updates fields and status", "[todo][task]") {
    TodoStore::instance().create_todo("test-session", make_item("Original"));

    TaskUpdateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto r = tool.call(
        nlohmann::json{
            {"taskId", "1"},
            {"subject", "Updated"},
            {"status", "in_progress"},
        },
        ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().data["task"]["content"] == "Updated");
    REQUIRE(r.value().data["task"]["status"] == "in_progress");

    auto got = TodoStore::instance().get_todo("test-session", "1");
    REQUIRE(got->content == "Updated");
    REQUIRE(got->status == core::todo::TodoStatus::InProgress);
}

TEST_CASE_METHOD(TodoFixture, "TaskUpdateTool status deleted removes task", "[todo][task]") {
    TodoStore::instance().create_todo("test-session", make_item("To remove"));

    TaskUpdateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto r = tool.call(nlohmann::json{{"taskId", "1"}, {"status", "deleted"}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().data["deleted"] == true);
    REQUIRE_FALSE(TodoStore::instance().get_todo("test-session", "1").has_value());
}

TEST_CASE_METHOD(TodoFixture, "TaskUpdateTool rejects unknown task", "[todo][task]") {
    TaskUpdateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json{{"taskId", "999"}, {"subject", "x"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ResourceNotFound);
}

TEST_CASE_METHOD(TodoFixture, "TaskUpdateTool rejects wrong-typed status", "[todo][task]") {
    TodoStore::instance().create_todo("test-session", make_item("Original"));

    TaskUpdateTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json{{"taskId", "1"}, {"status", 42}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);

    // 校验失败不产生任何写入（无部分更新、无事件丢失）
    auto got = TodoStore::instance().get_todo("test-session", "1");
    REQUIRE(got.has_value());
    REQUIRE(got->content == "Original");
    REQUIRE(got->status == core::todo::TodoStatus::Pending);
}

TEST_CASE_METHOD(TodoFixture, "TaskListTool lists all tasks", "[todo][task]") {
    TodoStore::instance().create_todo("test-session", make_item("A"));
    TodoStore::instance().create_todo("test-session", make_item("B"));

    TaskListTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    auto r = tool.call(nlohmann::json::object(), ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().data["tasks"].size() == 2);
    REQUIRE(r.value().data["tasks"][0]["content"] == "A");
    REQUIRE(r.value().data["tasks"][1]["content"] == "B");
}

// ============================================================
// SessionStore 持久化（todo 事件）
// ============================================================

TEST_CASE("SessionStore append_todo and load_todos roundtrip", "[todo][session]") {
    namespace fs = std::filesystem;
    static int seq = 0;
    fs::path tmp =
        fs::temp_directory_path() /
        ("workx_todo_test_" + std::to_string(++seq) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".jsonl");

    {
        auto store = std::make_shared<agent::session::SessionStore>(tmp.string(), "sess-1");
        REQUIRE(store->open());
        std::vector<core::todo::TodoItem> todos = {
            make_item("A", core::todo::TodoStatus::Completed),
            make_item("B", core::todo::TodoStatus::InProgress),
        };
        REQUIRE(store->append_todo(todos));
        // 第二次覆盖：模拟更新
        std::vector<core::todo::TodoItem> todos2 = {make_item("C")};
        REQUIRE(store->append_todo(todos2));
        store->close();
    }

    // 取最后一条 todo 事件
    auto loaded = agent::session::SessionStore::load_todos(tmp.string());
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].content == "C");
    REQUIRE(loaded[0].status == core::todo::TodoStatus::Pending);

    std::error_code ec;
    fs::remove(tmp, ec);
}
