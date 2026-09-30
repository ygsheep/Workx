/**
 * @file test_change_viewer.cpp
 * @brief 变更记录 tab（change_viewer）无头渲染 + 交互测试
 * @details 覆盖：空状态占位、文件分组头（路径 + 处数）、修改点列表（选中 ❯ 标记）、
 *          选中项目的 + hunk 行、e 展开完整 reasoning、命中区记录、
 *          组件交互（↑↓ 移动 / e 展开 / Enter 跳转回调）。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>

#include "core/utils/line_diff.h"
#include "widgets/change_viewer.h"

using namespace ftxtui;

namespace {

/// @brief 把元素渲染到固定尺寸 Screen 并返回文本
std::string render_elem(const ftxui::Element& e, int cols = 30, int rows = 24) {
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

/// @brief 构造一次文件修改（Edit：old→new）
FileChange make_change(std::string path, std::string purpose, std::string old_str,
                       std::string new_str, std::string reasoning = "") {
    FileChange ch;
    ch.file_path = std::move(path);
    ch.purpose = std::move(purpose);
    ch.reasoning = std::move(reasoning);
    ch.old_string = std::move(old_str);
    ch.new_string = std::move(new_str);
    ch.diff =
        agent::line_diff(agent::split_lines(ch.old_string), agent::split_lines(ch.new_string), 1);
    return ch;
}

ChangeViewState make_state() {
    ChangeViewState cv;
    cv.changes.push_back(
        make_change("src/main.cpp", "改用 bar() 计算 y", "auto y = foo();\n", "auto y = bar();\n"));
    cv.changes.push_back(make_change("src/main.cpp", "提取常量 kMaxRetry", "const int n = 3;\n",
                                     "const int kMaxRetry = 3;\n"));
    cv.changes.push_back(
        make_change("src/util.h", "修复空指针判断", "if (p) {}\n", "if (p != nullptr) {}\n"));
    cv.selected = 0;
    return cv;
}

}  // namespace

// ============================================================================
// 渲染：空状态 / 文件分组 / 修改点列表 / hunk / 目的展开
// ============================================================================

TEST_CASE("change viewer empty state shows placeholder", "[change_viewer][render]") {
    ChangeViewState cv;  // 无修改
    const auto text = render_elem(build_change_viewer(cv));
    REQUIRE(text.find("暂无文件修改") != std::string::npos);
}

TEST_CASE("change viewer groups changes by file with count", "[change_viewer][render]") {
    ChangeViewState cv = make_state();
    const auto text = render_elem(build_change_viewer(cv), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("src/main.cpp") != std::string::npos);
    REQUIRE(text.find("2 处修改") != std::string::npos);  // main.cpp 有 2 处
    REQUIRE(text.find("src/util.h") != std::string::npos);
    REQUIRE(text.find("1 处修改") != std::string::npos);  // util.h 有 1 处
}

TEST_CASE("change viewer marks selected change point with cursor", "[change_viewer][render]") {
    ChangeViewState cv = make_state();  // selected = 0
    const auto text = render_elem(build_change_viewer(cv), 40, 24);
    INFO("RENDERED:\n" << text);
    // 选中项行首 ❯；未选中项无 ❯
    REQUIRE(text.find("❯") != std::string::npos);
    REQUIRE(text.find("改用 bar() 计算 y") != std::string::npos);
}

TEST_CASE("change viewer shows purpose and hunk for selected change", "[change_viewer][render]") {
    ChangeViewState cv = make_state();  // selected = 0
    const auto text = render_elem(build_change_viewer(cv), 40, 24);
    INFO("RENDERED:\n" << text);
    // 选中项目的行 + hunk（+ 新增行）
    REQUIRE(text.find("目的：") != std::string::npos);
    REQUIRE(text.find("auto y = bar();") != std::string::npos);  // hunk 新内容
    // 未选中项不展开 hunk
    REQUIRE(text.find("kMaxRetry") != std::string::npos);  // 目的仍在列表
}

TEST_CASE("change viewer expands full reasoning on e", "[change_viewer][render]") {
    ChangeViewState cv = make_state();
    cv.changes[0].reasoning = "第一行思考\n第二行思考\n改用 bar() 计算 y";
    cv.purpose_expanded = true;
    const auto text = render_elem(build_change_viewer(cv), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("第一行思考") != std::string::npos);
    REQUIRE(text.find("第二行思考") != std::string::npos);
}

TEST_CASE("change viewer records hit boxes for change points", "[change_viewer][hit]") {
    ChangeViewState cv = make_state();
    std::deque<ChangeHit> hits;
    render_elem(build_change_viewer(cv, &hits), 40, 24);
    REQUIRE(hits.size() == 3);  // 3 个修改点
    for (std::size_t i = 0; i < hits.size(); ++i) {
        REQUIRE(hits[i].index == static_cast<int>(i));
        REQUIRE(hits[i].box.x_min >= 0);
        REQUIRE(hits[i].box.x_max >= hits[i].box.x_min);
        REQUIRE(hits[i].box.y_min >= 0);
        REQUIRE(hits[i].box.y_max >= hits[i].box.y_min);
    }
}

// ============================================================================
// 组件交互：↑↓ 移动 / e 展开 / Enter 跳转回调
// ============================================================================

TEST_CASE("change viewer component moves selection with arrows", "[change_viewer][interact]") {
    ChangeViewState cv = make_state();
    int jumps = 0;
    auto comp = make_change_viewer(&cv, [&] { ++jumps; });

    REQUIRE(comp->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(cv.selected == 1);
    REQUIRE(comp->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(cv.selected == 2);
    // 到底循环回首
    REQUIRE(comp->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(cv.selected == 0);
    // 向上循环
    REQUIRE(comp->OnEvent(ftxui::Event::ArrowUp));
    REQUIRE(cv.selected == 2);
    REQUIRE(jumps == 0);  // 移动不触发跳转
}

TEST_CASE("change viewer component toggles purpose expansion with e", "[change_viewer][interact]") {
    ChangeViewState cv = make_state();
    auto comp = make_change_viewer(&cv, [] {});

    REQUIRE(!cv.purpose_expanded);
    REQUIRE(comp->OnEvent(ftxui::Event::Character("e")));
    REQUIRE(cv.purpose_expanded);
    REQUIRE(comp->OnEvent(ftxui::Event::Character("E")));  // 大写同样切换
    REQUIRE(!cv.purpose_expanded);
}

TEST_CASE("change viewer component Enter triggers jump callback", "[change_viewer][interact]") {
    ChangeViewState cv = make_state();
    int jumps = 0;
    auto comp = make_change_viewer(&cv, [&] { ++jumps; });

    REQUIRE(comp->OnEvent(ftxui::Event::Return));
    REQUIRE(jumps == 1);
}

TEST_CASE("change viewer component ignores unrelated keys", "[change_viewer][interact]") {
    ChangeViewState cv = make_state();
    auto comp = make_change_viewer(&cv, [] {});
    // 普通字符（非 e）与 Esc 不消费
    REQUIRE(!comp->OnEvent(ftxui::Event::Character("x")));
    REQUIRE(!comp->OnEvent(ftxui::Event::Escape));
}
