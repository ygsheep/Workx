/**
 * @file test_jsonl_protocol.cpp
 * @brief JSONL 协议编解码单测
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>

#include <string>

#include "island/jsonl_protocol.h"

namespace {
bool close_enough(double a, double b, double eps = 1e-6) { return std::abs(a - b) < eps; }
}  // namespace

using island::Envelope;
using island::MsgKind;
using island::now_ts;
using island::parse_line;
using island::serialize_event;
using island::serialize_request;
using island::serialize_response;

TEST_CASE("jsonl: event serialize/parse round trip", "[island][jsonl]") {
    const std::string line = serialize_event("tool_call",
                                             nlohmann::json{
                                                 {"call_id", "call_1"},
                                                 {"tool_name", "Bash"},
                                             },
                                             42, 1712345678.500);

    REQUIRE(line.back() == '\n');
    auto env = parse_line(line);
    REQUIRE(env.has_value());
    REQUIRE(env->kind == MsgKind::Event);
    REQUIRE(env->type == "tool_call");
    REQUIRE(env->seq == 42);
    REQUIRE(close_enough(env->ts, 1712345678.500));
    REQUIRE(env->data["call_id"] == "call_1");
    REQUIRE(env->data["tool_name"] == "Bash");
}

TEST_CASE("jsonl: request serialize/parse", "[island][jsonl]") {
    const std::string req = serialize_request("refresh_balance", nlohmann::json{}, "req-1");
    auto renv = parse_line(req);
    REQUIRE(renv.has_value());
    REQUIRE(renv->kind == MsgKind::Request);
    REQUIRE(renv->type == "refresh_balance");
    REQUIRE(renv->id == "req-1");

    const std::string resp = serialize_response("req-1", false, nlohmann::json{{"error", "boom"}});
    auto senv = parse_line(resp);
    REQUIRE(senv.has_value());
    REQUIRE(senv->kind == MsgKind::Response);
    REQUIRE(senv->id == "req-1");
    REQUIRE_FALSE(senv->ok);
    REQUIRE(senv->data["error"] == "boom");
}

TEST_CASE("jsonl: parse rejects garbage", "[island][jsonl]") {
    REQUIRE_FALSE(parse_line("").has_value());
    REQUIRE_FALSE(parse_line("not json").has_value());
    REQUIRE_FALSE(parse_line("{\"kind\":\"bogus\"}").has_value());
    REQUIRE_FALSE(parse_line("[1,2,3]").has_value());
    REQUIRE_FALSE(parse_line("\n").has_value());
}

TEST_CASE("jsonl: event without seq defaults to 0", "[island][jsonl]") {
    const std::string line = "{\"kind\":\"event\",\"type\":\"x\",\"data\":{}}\n";
    auto env = parse_line(line);
    REQUIRE(env.has_value());
    REQUIRE(env->type == "x");
    REQUIRE(env->seq == 0);
    REQUIRE(env->data.is_object());
}

TEST_CASE("jsonl: now_ts returns positive unix seconds", "[island][jsonl]") {
    REQUIRE(now_ts() > 1'700'000'000.0);
}
