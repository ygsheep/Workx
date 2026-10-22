/**
 * @file test_message_node.cpp
 * @brief MessageNode 单元测试（ftxtui 无头逻辑）
 * @details 覆盖 find_tool 的 const/非 const 查找、命中/未命中、多工具块场景。
 */

#include <catch2/catch_test_macros.hpp>

#include "vm/message_node.h"

using namespace ftxtui;

TEST_CASE("MessageNode find_tool returns nullptr on empty", "[message_node][find_tool]") {
    MessageNode m;
    REQUIRE(m.find_tool("call_1") == nullptr);
}

TEST_CASE("MessageNode find_tool finds by call_id", "[message_node][find_tool]") {
    MessageNode m;
    m.tool_calls.push_back(ToolCallNode{.call_id = "a"});
    m.tool_calls.push_back(ToolCallNode{.call_id = "b"});

    REQUIRE(m.find_tool("a") != nullptr);
    REQUIRE(m.find_tool("b") != nullptr);
    REQUIRE(m.find_tool("a")->call_id == "a");
    REQUIRE(m.find_tool("b")->call_id == "b");
}

TEST_CASE("MessageNode find_tool mutates the matched node (non-const)",
          "[message_node][find_tool]") {
    MessageNode m;
    m.tool_calls.push_back(ToolCallNode{.call_id = "x"});
    auto* t = m.find_tool("x");
    REQUIRE(t != nullptr);
    t->done = true;
    t->result = "ok";
    REQUIRE(m.tool_calls[0].done == true);
    REQUIRE(m.tool_calls[0].result == "ok");
}

TEST_CASE("MessageNode find_tool const overload is read-only", "[message_node][find_tool]") {
    MessageNode m;
    m.tool_calls.push_back(ToolCallNode{.call_id = "y"});
    const MessageNode& cm = m;
    const auto* t = cm.find_tool("y");
    REQUIRE(t != nullptr);
    REQUIRE(t->call_id == "y");
    REQUIRE(cm.find_tool("missing") == nullptr);
}