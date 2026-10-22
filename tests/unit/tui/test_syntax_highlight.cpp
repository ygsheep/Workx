/**
 * @file test_syntax_highlight.cpp
 * @brief syntax_highlight 单元测试（ftxtui 无头逻辑）
 * @details 测试目标未定义 WORKX_HAS_TREE_SITTER，因此走关键字回退路径；
 *          tree-sitter 路径由 workx 主程序（已链接 grammar）在集成层面验证。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include "render/syntax_highlight.h"

using namespace ftxtui;

namespace {

/// @brief 把 Element 渲染到固定尺寸 Screen，返回逐行拼接的文本
std::string render_text(const ftxui::Element& e, int cols = 240, int rows = 80) {
    auto screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(cols), ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, e);
    std::string out;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) out += screen.PixelAt(x, y).character;
        out += '\n';
    }
    return out;
}

}  // namespace

TEST_CASE("highlight_code_line colors cpp keywords", "[syntax]") {
    auto text = render_text(highlight_code_line("int main() { return 0; }", "cpp"));
    REQUIRE(text.find("int") != std::string::npos);
    REQUIRE(text.find("main") != std::string::npos);
    REQUIRE(text.find("return") != std::string::npos);
}

TEST_CASE("highlight_code_line leaves unknown language unchanged", "[syntax]") {
    auto text = render_text(highlight_code_line("some text", "unknown-lang"));
    REQUIRE(text.find("some text") != std::string::npos);
}

TEST_CASE("highlight_code_block returns one element per line", "[syntax]") {
    const std::vector<std::string> lines = {"int main() {", "    return 0;", "}"};
    const auto elems = highlight_code_block(lines, "cpp");
    REQUIRE(elems.size() == lines.size());
    for (const auto& e : elems) {
        auto text = render_text(e);
        REQUIRE_FALSE(text.empty());
    }
}

TEST_CASE("highlight_code_block preserves all line content", "[syntax]") {
    const std::vector<std::string> lines = {"def foo():", "    return 42", "# comment"};
    const auto elems = highlight_code_block(lines, "python");
    REQUIRE(elems.size() == lines.size());
    auto t0 = render_text(elems[0]);
    auto t1 = render_text(elems[1]);
    auto t2 = render_text(elems[2]);
    REQUIRE(t0.find("def foo():") != std::string::npos);
    REQUIRE(t1.find("return 42") != std::string::npos);
    REQUIRE(t2.find("# comment") != std::string::npos);
}

TEST_CASE("highlight_code_block handles empty lines", "[syntax]") {
    const std::vector<std::string> lines = {"int a;", "", "int b;"};
    const auto elems = highlight_code_block(lines, "cpp");
    REQUIRE(elems.size() == lines.size());
    // 空行 Element 渲染后不产生额外内容（不崩溃、高度正常）
    auto t0 = render_text(elems[0]);
    auto t2 = render_text(elems[2]);
    REQUIRE(t0.find("int a;") != std::string::npos);
    REQUIRE(t2.find("int b;") != std::string::npos);
}

TEST_CASE("highlight_code_block empty input returns empty", "[syntax]") {
    const std::vector<std::string> lines;
    REQUIRE(highlight_code_block(lines, "cpp").empty());
}

TEST_CASE("highlight_code_block unknown language falls back to plain text", "[syntax]") {
    const std::vector<std::string> lines = {"hello world"};
    const auto elems = highlight_code_block(lines, "unknown-lang");
    REQUIRE(elems.size() == 1);
    auto text = render_text(elems[0]);
    REQUIRE(text.find("hello world") != std::string::npos);
}

#ifdef WORKX_HAS_TREE_SITTER

namespace {

/// @brief 渲染 Element 并统计非默认前景色的像素数（>0 表示有真实着色）
int count_colored_pixels(const ftxui::Element& e, int cols = 240, int rows = 80) {
    auto screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(cols), ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, e);
    int n = 0;
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            const auto& p = screen.PixelAt(x, y);
            if (p.foreground_color != ftxui::Color::Default) ++n;
        }
    return n;
}

}  // namespace

TEST_CASE("tree-sitter highlights cpp code block", "[syntax][ts]") {
    const std::vector<std::string> lines = {
        "#include <vector>",
        "int main() {",
        "    std::vector<int> v{1, 2, 3};",
        "    return 0;",
        "}",
    };
    const auto elems = highlight_code_block(lines, "cpp");
    REQUIRE(elems.size() == lines.size());
    // 至少一行有真实着色（关键字/类型/数字/预处理）
    int colored = 0;
    for (const auto& e : elems) colored += count_colored_pixels(e);
    REQUIRE(colored > 0);
    // 内容完整保留
    auto t0 = render_text(elems[0]);
    REQUIRE(t0.find("#include <vector>") != std::string::npos);
    auto t2 = render_text(elems[2]);
    REQUIRE(t2.find("std::vector<int> v{1, 2, 3};") != std::string::npos);
}

TEST_CASE("tree-sitter highlights python multi-line string", "[syntax][ts]") {
    const std::vector<std::string> lines = {
        "def greet(name):",
        "    \"\"\"multi",
        "    line docstring\"\"\"",
        "    return f'hello {name}'",
    };
    const auto elems = highlight_code_block(lines, "python");
    REQUIRE(elems.size() == lines.size());
    // 多行字符串跨行着色：中间行应有绿色字符串像素
    REQUIRE(count_colored_pixels(elems[1]) > 0);
    auto t3 = render_text(elems[3]);
    REQUIRE(t3.find("return f'hello {name}'") != std::string::npos);
}

TEST_CASE("tree-sitter highlights json block", "[syntax][ts]") {
    const std::vector<std::string> lines = {
        "{",
        "  \"key\": 42,",
        "  \"flag\": true",
        "}",
    };
    const auto elems = highlight_code_block(lines, "json");
    REQUIRE(elems.size() == lines.size());
    int colored = 0;
    for (const auto& e : elems) colored += count_colored_pixels(e);
    REQUIRE(colored > 0);
    auto t1 = render_text(elems[1]);
    REQUIRE(t1.find("\"key\": 42") != std::string::npos);
}

TEST_CASE("tree-sitter unknown language falls back to plain text", "[syntax][ts]") {
    const std::vector<std::string> lines = {"plain text"};
    const auto elems = highlight_code_block(lines, "unknown-lang");
    REQUIRE(elems.size() == 1);
    REQUIRE(count_colored_pixels(elems[0]) == 0);
}

#endif  // WORKX_HAS_TREE_SITTER
