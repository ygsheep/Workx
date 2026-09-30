/**
 * @file test_view_model_todo.cpp
 * @brief #24：ActionTodoUpdate 更新 SidebarModel.todos（触发重绘）
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "bridge/action.h"
#include "core/todo/todo_item.h"
#include "vm/view_model.h"

using namespace ftxtui;

namespace {

core::todo::TodoItem make_item(const std::string& content,
                               core::todo::TodoStatus status = core::todo::TodoStatus::Pending) {
    core::todo::TodoItem item;
    item.id = "1";
    item.content = content;
    item.active_form = content + "ing";
    item.status = status;
    return item;
}

}  // namespace

TEST_CASE("ViewModel ActionTodoUpdate stores todos in sidebar", "[view_model][todo]") {
    ViewModel vm;
    REQUIRE(vm.sidebar.todos.empty());

    std::vector<core::todo::TodoItem> todos = {
        make_item("Run tests", core::todo::TodoStatus::InProgress),
        make_item("Write docs", core::todo::TodoStatus::Completed),
    };
    REQUIRE(vm.apply(ActionTodoUpdate{.session_id = "s1", .todos = todos}));
    REQUIRE(vm.sidebar.todos.size() == 2);
    REQUIRE(vm.sidebar.todos[0].content == "Run tests");
    REQUIRE(vm.sidebar.todos[0].status == core::todo::TodoStatus::InProgress);
    REQUIRE(vm.sidebar.todos[1].status == core::todo::TodoStatus::Completed);
}

TEST_CASE("ViewModel ActionTodoUpdate identical snapshot returns false", "[view_model][todo]") {
    ViewModel vm;
    std::vector<core::todo::TodoItem> todos = {make_item("A")};
    REQUIRE(vm.apply(ActionTodoUpdate{.session_id = "s1", .todos = todos}));
    // 相同快照 → 无变化，不触发重绘
    REQUIRE_FALSE(vm.apply(ActionTodoUpdate{.session_id = "s1", .todos = todos}));
}

TEST_CASE("ViewModel ActionTodoUpdate empty snapshot clears todos", "[view_model][todo]") {
    ViewModel vm;
    std::vector<core::todo::TodoItem> todos = {make_item("A")};
    vm.apply(ActionTodoUpdate{.session_id = "s1", .todos = todos});
    REQUIRE(vm.apply(ActionTodoUpdate{.session_id = "s1", .todos = {}}));
    REQUIRE(vm.sidebar.todos.empty());
}
