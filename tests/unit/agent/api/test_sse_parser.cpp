/**
 * @file test_sse_parser.cpp
 * @brief SSEParser 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/api/sse_parser.hpp"

using namespace agent;

TEST_CASE("SSEParser basic event", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("data: hello\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello");
}

TEST_CASE("SSEParser multiple events", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("data: first\n\ndata: second\n\n");

    REQUIRE(events.size() == 2);
    REQUIRE(events[0].data == "first");
    REQUIRE(events[1].data == "second");
}

TEST_CASE("SSEParser event with id", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("id: 42\ndata: test\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].id == "42");
    REQUIRE(events[0].data == "test");
}

TEST_CASE("SSEParser event field", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("event: custom\ndata: payload\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].event == "custom");
    REQUIRE(events[0].data == "payload");
}

TEST_CASE("SSEParser split data lines", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("data: line1\ndata: line2\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "line1\nline2");
}

TEST_CASE("SSEParser incremental parsing", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("data: hel");
    REQUIRE(events.empty());

    parser.parse("lo\n\n");
    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "hello");
}

TEST_CASE("SSEParser ignore comments", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse(": this is a comment\ndata: real\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].data == "real");
}

TEST_CASE("SSEParser anthropic-style named events", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    // Anthropic 使用 event: + data: 组合
    parser.parse(
        "event: content_block_delta\ndata: "
        "{\"type\":\"content_block_delta\",\"delta\":{\"text\":\"Hello\"}}\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].event == "content_block_delta");
    REQUIRE(events[0].data == "{\"type\":\"content_block_delta\",\"delta\":{\"text\":\"Hello\"}}");
}

TEST_CASE("SSEParser anthropic message_stop event", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].event == "message_stop");
    REQUIRE(events[0].data == "{\"type\":\"message_stop\"}");
}

TEST_CASE("SSEParser event with data and no event field - OpenAI style", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    // OpenAI 格式：只有 data，没有 event
    parser.parse("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n");

    REQUIRE(events.size() == 1);
    REQUIRE(events[0].event.empty());  // 没有 event 字段
    REQUIRE(events[0].data.find("Hello") != std::string::npos);
}

TEST_CASE("SSEParser empty data", "[sse]") {
    std::vector<SSEEvent> events;

    SSEParser parser([&](const SSEEvent& event) { events.push_back(event); });

    parser.parse("data:\n\n");

    // 空数据事件是否触发取决于实现
    REQUIRE(events.size() <= 1);
}
