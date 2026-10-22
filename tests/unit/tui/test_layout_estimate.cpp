/**
 * @file test_layout_estimate.cpp
 * @brief 布局高度估算单测（A3 单一布局源）
 * @details estimate_markdown_height / estimate_message_height 必须与渲染
 *          （build_markdown / build_message）逐行一致：渲染后读取
 *          Node::requirement().min_y（元素完全绘制所需最小高度），
 *          与估算值对拍。任何布局改动造成漂移时此测试先红。
 */

#include <catch2/catch_test_macros.hpp>

#include <ftxui/dom/node.hpp>

#include "render/markdown_to_elements.h"
#include "vm/message_node.h"

using namespace ftxtui;

namespace {

/// @brief 渲染消息并返回其最小需求高度（行数）
int rendered_height(const MessageNode& msg) {
    auto node = build_message(msg, 240);
    node->ComputeRequirement();
    return node->requirement().min_y;
}

/// @brief 渲染 markdown 并返回其最小需求高度（行数）
int rendered_md_height(std::string_view text) {
    auto node = build_markdown(text, 240);
    node->ComputeRequirement();
    return node->requirement().min_y;
}

MessageNode assistant_msg(std::string text) {
    MessageNode m;
    m.role = MsgRole::Assistant;
    m.text = std::move(text);
    m.sealed = true;
    return m;
}

}  // namespace

// ============================================================================
// estimate_markdown_height 与渲染对拍
// ============================================================================

TEST_CASE("estimate_markdown_height matches plain paragraphs", "[layout][markdown]") {
    const std::string md = "line one\nline two\n\nline three";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) ==
            6);  // 3 物理行 × 段落行距2；空行（emptyElement）不占行
}

TEST_CASE("estimate_markdown_height matches code block with language tag", "[layout][markdown]") {
    const std::string md = "```cpp\nint a;\nint b;\n```";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) == 5);  // 2 代码行 + 1 语言标签行 + 2 上下留白
}

TEST_CASE("estimate_markdown_height matches code block without language", "[layout][markdown]") {
    const std::string md = "```\nline a\nline b\n```";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) == 4);  // 2 代码行 + 2 上下留白
}

TEST_CASE("estimate_markdown_height matches empty code block (not rendered)",
          "[layout][markdown]") {
    const std::string md = "```\n```";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) == 0);
}

TEST_CASE("estimate_markdown_height matches blank-only text", "[layout][markdown]") {
    const std::string md = "   \n\t\n  ";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) == 0);
}

TEST_CASE("estimate_markdown_height matches empty input", "[layout][markdown]") {
    // build_markdown("") → text("")，FTXUI 空文本 min_y=1
    REQUIRE(estimate_markdown_height("", 240) == rendered_md_height(""));
    REQUIRE(estimate_markdown_height("", 240) == 1);
}

TEST_CASE("estimate_markdown_height matches mixed blocks", "[layout][markdown]") {
    const std::string md =
        "# Title\n"
        "- item\n"
        "```cpp\ncode\n```\n"
        "| a | b |\n"
        "---\n"
        "para\n"
        "\n"
        "```\n```\n";  // 空代码块不渲染
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    // 标题 1 + 列表 1 + 代码(1+1+2=4) + 孤立|行 1×行距2=2 + 分隔线 1 + 段落 1×行距2=2 = 11
    REQUIRE(estimate_markdown_height(md, 240) == 11);
}

TEST_CASE("estimate_markdown_height matches table block", "[layout][markdown]") {
    const std::string md =
        "| 场景 | 复杂度 |\n"
        "| --- | --- |\n"
        "| 平均 | O(n log n) |\n"
        "| 最坏 | O(n^2) |";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    // 顶边框 1 + 表头 1 + 中边框 1 + 数据 2 + 底边框 1 = 6
    REQUIRE(estimate_markdown_height(md, 240) == 6);
}

TEST_CASE("estimate_markdown_height matches table with alignment separator", "[layout][markdown]") {
    const std::string md =
        "| a | b | c |\n"
        "| :--- | :---: | ---: |\n"
        "| 1 | 2 | 3 |";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) == 5);  // 4 边框 + 1 数据
}

TEST_CASE("estimate_markdown_height matches orphan pipe line (degraded)", "[layout][markdown]") {
    // 无分隔行的孤立 | 行：降级为普通段落 1 行（×行距2）
    const std::string md = "| a | b |";
    REQUIRE(estimate_markdown_height(md, 240) == rendered_md_height(md));
    REQUIRE(estimate_markdown_height(md, 240) == 2);
}

// ============================================================================
// estimate_message_height 与渲染对拍
// ============================================================================

TEST_CASE("estimate_message_height matches plain assistant message", "[layout][message]") {
    MessageNode m = assistant_msg("hello\nworld");
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    REQUIRE(estimate_message_height(m, 240) == 5);  // 正文 2 物理行×行距2 = 4 + 操作按钮栏 1
}

TEST_CASE("estimate_message_height matches empty assistant message", "[layout][message]") {
    MessageNode m = assistant_msg("");
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    REQUIRE(estimate_message_height(m, 240) == 0);  // 无正文 → 无操作按钮栏
}

TEST_CASE("estimate_message_height matches streaming empty assistant message",
          "[layout][message]") {
    MessageNode m = assistant_msg("");
    m.streaming = true;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    REQUIRE(estimate_message_height(m, 240) == 2);  // 正文行 1 + 流式游标 1（无操作按钮栏）
}

TEST_CASE("estimate_message_height hides buttons on thinking-only message", "[layout][message]") {
    MessageNode m = assistant_msg("");
    m.reasoned = true;
    m.reasoning = "thinking line";
    m.reasoning_expanded = false;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 思考卡 3（边框 2 + 头 1），无正文 → 无操作按钮栏
    REQUIRE(estimate_message_height(m, 240) == 3);
}

TEST_CASE("estimate_message_height hides buttons on tool-only message", "[layout][message]") {
    MessageNode m = assistant_msg("");
    ToolCallNode t;
    t.tool_name = "read_file";
    t.call_id = "c1";
    t.done = true;
    t.expanded = false;
    m.tool_calls.push_back(t);
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 工具卡 3 + 分隔（卡前 1 + 卡后 1 = 2），无正文 → 无操作按钮栏
    REQUIRE(estimate_message_height(m, 240) == 5);
}

TEST_CASE("estimate_message_height matches user message", "[layout][message]") {
    MessageNode m;
    m.role = MsgRole::User;
    m.text = "hi\n```cpp\nx\n```";
    m.sealed = true;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 紧凑段落：私信 1 行×1 + 代码块（1 代码行 + 1 lang 行，compact 无上下留白 = 2） = 3
    // 用户消息块自带上下各 1 空行（build_message 的 UserMessageBox 布局约束，+2） → 5
    REQUIRE(estimate_message_height(m, 240) == 5);
}

TEST_CASE("estimate_message_height matches user message with empty text", "[layout][message]") {
    MessageNode m;
    m.role = MsgRole::User;
    m.sealed = true;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    REQUIRE(estimate_message_height(m, 240) == 0);
}

TEST_CASE("estimate_message_height matches notice assistant (no action bar)", "[layout][message]") {
    MessageNode m = assistant_msg("notice text");
    m.notice = true;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 正文 1×行距2=2，notice 不显示操作按钮栏 → 2
    REQUIRE(estimate_message_height(m, 240) == 2);
}

TEST_CASE("estimate_message_height matches reasoning card collapsed", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    m.reasoned = true;
    m.reasoning = "thinking line";
    m.reasoning_expanded = false;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 思考卡 3（边框 2 + 头 1）+ 正文 1×行距2=2 + 操作按钮栏 1 = 6
    REQUIRE(estimate_message_height(m, 240) == 6);
}

TEST_CASE("estimate_message_height matches reasoning card expanded", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    m.reasoned = true;
    m.reasoning = "think a\nthink b";
    m.reasoning_expanded = true;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 思考卡 3 + 展开 reasoning 2 物理行×行距2=4 + 正文 1×行距2=2 + 操作按钮栏 1 = 10
    REQUIRE(estimate_message_height(m, 240) == 10);
}

TEST_CASE("estimate_message_height matches tool card", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    ToolCallNode t;
    t.tool_name = "read_file";
    t.call_id = "c1";
    t.done = true;
    t.expanded = true;
    t.result = "line1\nline2";
    m.tool_calls.push_back(t);
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 工具卡 3(边框2+头1) + 展开 2 + 正文 1×行距2=2 + 暂不计算 result 包裹留白 + 操作按钮栏 1 = 11
    REQUIRE(estimate_message_height(m, 240) == 11);
}

TEST_CASE("estimate_message_height matches tool card collapsed", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    ToolCallNode t;
    t.tool_name = "read_file";
    t.call_id = "c1";
    t.done = true;
    t.expanded = false;
    m.tool_calls.push_back(t);
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 折叠工具卡：正文 1×2=2 + 卡 3 行（无非展开内容）+ 卡前空 1 + 按钮栏 1 = 7
    REQUIRE(estimate_message_height(m, 240) == 7);
}

TEST_CASE("estimate_message_height matches tool card with file path header", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    ToolCallNode t;
    t.tool_name = "read_file";
    t.call_id = "c1";
    t.arguments = R"({"file_path": "src/app.cpp"})";
    t.done = true;
    t.expanded = true;
    t.result = "line1\nline2";
    m.tool_calls.push_back(t);
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 展开工具卡：正文 1×2=2 + 卡：头 1 + 路径行 1 + 结果（status 1+vPad 2+2 行=...见下）+ 卡前空 1
    // + 按钮栏 1 = 14
    REQUIRE(estimate_message_height(m, 240) == 14);
}

TEST_CASE("estimate_message_height ignores tool card non-json arguments", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    ToolCallNode t;
    t.tool_name = "Bash";
    t.call_id = "c1";
    t.arguments = "not json";
    t.done = true;
    t.expanded = true;
    t.result = "line1";
    m.tool_calls.push_back(t);
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 展开工具卡（无路径行）：正文 1×2=2 + 卡：头 1 + 结果（status 1+vPad 2+1 行）+ 卡前空 1 +
    // 按钮栏 1 = 9
    REQUIRE(estimate_message_height(m, 240) == 9);
}

TEST_CASE("estimate_message_height matches two adjacent tool cards", "[layout][message]") {
    MessageNode m = assistant_msg("answer");
    ToolCallNode t1;
    t1.tool_name = "read_file";
    t1.call_id = "c1";
    t1.done = true;
    t1.expanded = false;
    m.tool_calls.push_back(t1);
    ToolCallNode t2;
    t2.tool_name = "grep";
    t2.call_id = "c2";
    t2.done = true;
    t2.expanded = false;
    m.tool_calls.push_back(t2);
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 两张折叠卡：正文 1×2=2 + 卡 3+3 + 卡前空 1 + 末卡后空 1 + 按钮栏 1 = 10
    REQUIRE(estimate_message_height(m, 240) == 10);
}

TEST_CASE("estimate_message_height matches error message", "[layout][message]") {
    MessageNode m;
    m.role = MsgRole::Error;
    m.text = "boom";
    m.sealed = true;
    REQUIRE(estimate_message_height(m, 240) == rendered_height(m));
    // 错误头 1 + 正文 1×行距2=2 = 3
    REQUIRE(estimate_message_height(m, 240) == 3);
}