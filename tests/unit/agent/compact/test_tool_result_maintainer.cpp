/**
 * @file test_tool_result_maintainer.cpp
 * @brief tool_result 两级维护单元测试（DS_CACHE H-1）
 */

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "agent/compact/tool_result_maintainer.h"
#include "agent/api/chat_types.h"

using namespace agent::compact;
using agent::ChatMessage;

// ============================================================
// 工具分类
// ============================================================

TEST_CASE("is_read_only_tool classification", "[compact][tool_result][ds_cache]") {
    SECTION("read-only tools") {
        REQUIRE(is_read_only_tool("Read"));
        REQUIRE(is_read_only_tool("Glob"));
        REQUIRE(is_read_only_tool("Grep"));
        REQUIRE(is_read_only_tool("LS"));
        REQUIRE(is_read_only_tool("WebFetch"));
    }

    SECTION("side-effecting tools") {
        REQUIRE_FALSE(is_read_only_tool("Bash"));
        REQUIRE_FALSE(is_read_only_tool("Write"));
        REQUIRE_FALSE(is_read_only_tool("Edit"));
        REQUIRE_FALSE(is_read_only_tool("PowerShell"));
    }

    SECTION("unknown tool defaults to read-only") { REQUIRE(is_read_only_tool("UnknownTool")); }
}

TEST_CASE("select_strategy_by_tool_name", "[compact][tool_result][ds_cache]") {
    SECTION("read-only tool gets read-only strategy (head>tail)") {
        const auto& s = select_strategy_by_tool_name("Read");
        REQUIRE(s.head_lines > s.tail_lines);
    }

    SECTION("side-effecting tool gets equal strategy (head==tail)") {
        const auto& s = select_strategy_by_tool_name("Bash");
        REQUIRE(s.head_lines == s.tail_lines);
    }
}

// ============================================================
// snip_tool_result — 行级截短
// ============================================================

TEST_CASE("snip_tool_result line-level", "[compact][tool_result][ds_cache]") {
    SECTION("long content with many lines: snipped with placeholder") {
        // 生成 100 行内容
        std::string content;
        for (int i = 0; i < 100; ++i) {
            content += "line " + std::to_string(i) + "\n";
        }
        auto msg = ChatMessage::tool_result("call_1", "Read", content);

        SnipStrategy strategy{5, 3, 10'000, 2'000};  // head 5 行, tail 3 行
        int saved = snip_tool_result(msg, strategy);

        REQUIRE(saved > 0);
        REQUIRE(msg.content.find("[snipped tool result") != std::string::npos);
        REQUIRE(msg.content.find("line 0\n") != std::string::npos);   // 头部保留
        REQUIRE(msg.content.find("line 4\n") != std::string::npos);   // 头部最后行
        REQUIRE(msg.content.find("line 99\n") != std::string::npos);  // 尾部保留
        REQUIRE(msg.content.find("line 50\n") == std::string::npos);  // 中段被删
    }

    SECTION("short content: not snipped") {
        auto msg = ChatMessage::tool_result("call_1", "Read", "short result");
        SnipStrategy strategy{80, 12, 10'000, 2'000};
        int saved = snip_tool_result(msg, strategy);
        REQUIRE(saved == 0);
        REQUIRE(msg.content == "short result");
    }

    SECTION("non-tool role: no-op") {
        auto msg = ChatMessage::assistant("assistant content");
        SnipStrategy strategy{5, 3, 100, 100};
        int saved = snip_tool_result(msg, strategy);
        REQUIRE(saved == 0);
    }

    SECTION("empty content: no-op") {
        auto msg = ChatMessage::tool_result("call_1", "Read", "");
        int saved = snip_tool_result(msg, SnipStrategy{5, 3, 100, 100});
        REQUIRE(saved == 0);
    }
}

// ============================================================
// snip_tool_result — 字符级 fallback
// ============================================================

TEST_CASE("snip_tool_result char-level fallback", "[compact][tool_result][ds_cache]") {
    SECTION("few lines but very long: char-level fallback") {
        // 单行超长内容（无换行）
        std::string content(20'000, 'x');
        auto msg = ChatMessage::tool_result("call_1", "Read", content);

        SnipStrategy strategy{80, 12, 1'000, 500};  // head_chars=1000, tail_chars=500
        int saved = snip_tool_result(msg, strategy);

        REQUIRE(saved > 0);
        REQUIRE(msg.content.find("[snipped tool result") != std::string::npos);
        REQUIRE(msg.content.find("chars elided") != std::string::npos);
    }
}

// ============================================================
// 幂等性：已截短的消息不重复截短
// ============================================================

TEST_CASE("snip_tool_result idempotent (already elided)", "[compact][tool_result][ds_cache]") {
    SECTION("already snipped: no-op") {
        auto msg = ChatMessage::tool_result(
            "call_1", "Read", "head\n[snipped tool result — 50 lines elided]\ntail\n");
        std::string original = msg.content;
        int saved = snip_tool_result(msg, SnipStrategy{5, 3, 100, 100});
        REQUIRE(saved == 0);
        REQUIRE(msg.content == original);
    }

    SECTION("already pruned: no-op for snip") {
        auto msg =
            ChatMessage::tool_result("call_1", "Read", "[elided tool result — 1000 bytes]\n");
        int saved = snip_tool_result(msg, SnipStrategy{5, 3, 100, 100});
        REQUIRE(saved == 0);
    }
}

// ============================================================
// prune_tool_result
// ============================================================

TEST_CASE("prune_tool_result", "[compact][tool_result][ds_cache]") {
    SECTION("replace entire content with placeholder") {
        std::string long_content(5'000, 'x');
        auto msg = ChatMessage::tool_result("call_1", "Bash", long_content);

        int saved = prune_tool_result(msg, "");
        REQUIRE(saved > 0);
        REQUIRE(msg.content.find("[elided tool result") != std::string::npos);
        REQUIRE(msg.content.find("5000 bytes") != std::string::npos);
        REQUIRE(msg.content.size() < long_content.size());
    }

    SECTION("archive_dir appears in placeholder") {
        auto msg = ChatMessage::tool_result("call_1", "Bash", "some content here");
        prune_tool_result(msg, "/tmp/archive");
        REQUIRE(msg.content.find("/tmp/archive") != std::string::npos);
    }

    SECTION("already elided: no-op") {
        auto msg = ChatMessage::tool_result("call_1", "Bash", "[elided tool result — 100 bytes]\n");
        std::string original = msg.content;
        int saved = prune_tool_result(msg);
        REQUIRE(saved == 0);
        REQUIRE(msg.content == original);
    }

    SECTION("non-tool role: no-op") {
        auto msg = ChatMessage::user("hello");
        int saved = prune_tool_result(msg);
        REQUIRE(saved == 0);
    }
}

// ============================================================
// 不变量：不改 tool_call_id
// ============================================================

TEST_CASE("snip/prune preserve tool_call_id", "[compact][tool_result][ds_cache]") {
    SECTION("snip preserves tool_call_id") {
        std::string content;
        for (int i = 0; i < 50; ++i) content += "line\n";
        auto msg = ChatMessage::tool_result("call_abc_123", "Read", content);
        snip_tool_result(msg, SnipStrategy{5, 3, 100, 100});
        REQUIRE(msg.tool_call_id == "call_abc_123");
    }

    SECTION("prune preserves tool_call_id") {
        auto msg = ChatMessage::tool_result("call_xyz", "Bash", std::string(1000, 'x'));
        prune_tool_result(msg);
        REQUIRE(msg.tool_call_id == "call_xyz");
    }
}
