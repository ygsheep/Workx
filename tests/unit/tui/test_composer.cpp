/**
 * @file test_composer.cpp
 * @brief 输入组件快捷键行为无头单元测试
 * @details 覆盖：Ctrl+Enter（文件面板插入 @路径 引用 / 无面板时提交并请求立即冲刷）
 *          与 Enter（直接 nvim 打开）的区分，以及 Windows 补丁产生的
 *          kitty 序列 \x1b[13;5u 被解析为 Special 事件。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/terminal_input_parser.hpp>

#include "widgets/composer.h"

using namespace ftxtui;

// ============================================================================
// Windows 补丁：Ctrl+Enter 改写为 kitty 序列 \x1b[13;5u → Special 事件
// ============================================================================

TEST_CASE("parser maps ctrl-enter kitty sequence to Special event", "[composer][parser]") {
    std::vector<ftxui::Event> received;
    ftxui::TerminalInputParser parser([&](ftxui::Event e) { received.push_back(std::move(e)); });
    const std::string seq = "\x1b[13;5u";
    for (char c : seq) parser.Add(c);
    REQUIRE(received.size() == 1);
    REQUIRE_FALSE(received[0].is_character());
    REQUIRE(received[0].input() == seq);
}

TEST_CASE("parser maps shift-enter kitty sequence to Special event", "[composer][parser]") {
    std::vector<ftxui::Event> received;
    ftxui::TerminalInputParser parser([&](ftxui::Event e) { received.push_back(std::move(e)); });
    const std::string seq = "\x1b[13;2u";
    for (char c : seq) parser.Add(c);
    REQUIRE(received.size() == 1);
    REQUIRE_FALSE(received[0].is_character());
    REQUIRE(received[0].input() == seq);
}

TEST_CASE("parser maps ctrl-left kitty sequence to Special event", "[composer][parser]") {
    std::vector<ftxui::Event> received;
    ftxui::TerminalInputParser parser([&](ftxui::Event e) { received.push_back(std::move(e)); });
    const std::string seq = "\x1b[1;5D";
    for (char c : seq) parser.Add(c);
    REQUIRE(received.size() == 1);
    REQUIRE_FALSE(received[0].is_character());
    REQUIRE(received[0].input() == seq);
}

TEST_CASE("parser maps ctrl-right kitty sequence to Special event", "[composer][parser]") {
    std::vector<ftxui::Event> received;
    ftxui::TerminalInputParser parser([&](ftxui::Event e) { received.push_back(std::move(e)); });
    const std::string seq = "\x1b[1;5C";
    for (char c : seq) parser.Add(c);
    REQUIRE(received.size() == 1);
    REQUIRE_FALSE(received[0].is_character());
    REQUIRE(received[0].input() == seq);
}

// ============================================================================
// composer：Ctrl+Enter / Enter 在提示面板激活时的行为
// ============================================================================

namespace {

/// @brief 构造带 mock 回调的 composer，返回组件与回调记录
struct ComposerHarness {
    std::string buf = "@src";
    size_t cursor = 4;
    bool panel_active = true;
    bool insert_called = false;
    bool enter_called = false;
    bool submit_called = false;
    bool ctrl_submit_called = false;
    ComposerOptions opt;  // 必须与组件同生命周期（make_composer 捕获其引用）
    ftxui::Component comp;

    explicit ComposerHarness() {
        opt.buffer = &buf;
        opt.cursor = &cursor;
        opt.suggest_active = [this] { return panel_active; };
        opt.suggest_enter_insert = [this] {
            insert_called = true;
            return true;
        };
        opt.suggest_enter = [this] {
            enter_called = true;
            return true;
        };
        opt.on_submit = [this](const std::string&) { submit_called = true; };
        opt.on_submit_ctrl = [this](const std::string&) { ctrl_submit_called = true; };
        comp = make_composer(opt);
    }
};

}  // namespace

TEST_CASE("composer Ctrl+Enter calls suggest_enter_insert when panel active",
          "[composer][suggest]") {
    ComposerHarness h;
    const bool handled = h.comp->OnEvent(ftxui::Event::Special("\x1b[13;5u"));
    REQUIRE(handled);
    REQUIRE(h.insert_called);
    REQUIRE_FALSE(h.enter_called);
    REQUIRE_FALSE(h.submit_called);
}

TEST_CASE("composer Enter calls suggest_enter when panel active", "[composer][suggest]") {
    ComposerHarness h;
    const bool handled = h.comp->OnEvent(ftxui::Event::Return);
    REQUIRE(handled);
    REQUIRE(h.enter_called);
    REQUIRE_FALSE(h.insert_called);
    REQUIRE_FALSE(h.submit_called);
}

TEST_CASE("composer Ctrl+Enter submits via on_submit_ctrl without panel", "[composer][suggest]") {
    ComposerHarness h;
    h.panel_active = false;
    h.buf = "queued msg";
    h.cursor = h.buf.size();
    const bool handled = h.comp->OnEvent(ftxui::Event::Special("\x1b[13;5u"));
    REQUIRE(handled);
    REQUIRE(h.ctrl_submit_called);
    REQUIRE_FALSE(h.insert_called);
    REQUIRE_FALSE(h.submit_called);
    REQUIRE(h.buf.empty());  // 提交后清空输入缓冲
}

TEST_CASE("composer Shift+Enter inserts newline without submitting", "[composer][suggest]") {
    ComposerHarness h;
    const bool handled = h.comp->OnEvent(ftxui::Event::Special("\x1b[13;2u"));
    REQUIRE(handled);
    REQUIRE(h.buf == "@src\n");
    REQUIRE(h.cursor == 5);
    REQUIRE_FALSE(h.submit_called);
}
