/**
 * @file test_file_viewer.cpp
 * @brief 文件查看器（/view 只读）无头渲染测试
 * @details 覆盖：空状态占位、路径栏（路径/行数/语言）、行号列、虚拟化滚动切片、
 *          语言推断（lang_from_path）。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include "render/markdown_to_elements.h"
#include "render/image_view.h"
#include "widgets/file_viewer.h"

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

FileViewState make_file(std::vector<std::string> lines, std::string path = "src/main.cpp",
                        std::string lang = "cpp", int scroll = 0) {
    FileViewState f;
    f.path = std::move(path);
    f.lines = std::move(lines);
    f.lang = std::move(lang);
    f.scroll = scroll;
    return f;
}

}  // namespace

TEST_CASE("file viewer empty state shows placeholder", "[file_viewer][render]") {
    FileViewState f;  // path 为空
    const auto text = render_elem(build_file_viewer(f));
    REQUIRE(text.find("暂无打开的文件") != std::string::npos);
}

TEST_CASE("file viewer shows path bar with line count and lang", "[file_viewer][render]") {
    FileViewState f = make_file({"int main() {", "  return 0;", "}"});
    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("src/main.cpp") != std::string::npos);
    REQUIRE(text.find("3 行") != std::string::npos);
    REQUIRE(text.find("cpp") != std::string::npos);
}

TEST_CASE("file viewer shows line numbers and content", "[file_viewer][render]") {
    FileViewState f = make_file({"int main() {", "  return 0;", "}"});
    const auto text = render_elem(build_file_viewer(f), 40, 24);
    REQUIRE(text.find("1") != std::string::npos);
    REQUIRE(text.find("2") != std::string::npos);
    REQUIRE(text.find("3") != std::string::npos);
    REQUIRE(text.find("int main() {") != std::string::npos);
    REQUIRE(text.find("return 0") != std::string::npos);
}

TEST_CASE("file viewer scrolls to offset slice", "[file_viewer][render]") {
    FileViewState f =
        make_file({"line0", "line1", "line2", "line3", "line4", "line5"}, "f.txt", "", 3);
    // 显式视口高度 3 行（绕过终端探测，保证 scroll=3 → line0/1/2 出视口）
    const auto text = render_elem(build_file_viewer(f, 40, 3), 40, 24);
    INFO("RENDERED:\n" << text);
    // scroll=3：从 line3 开始显示，line0/1/2 不可见
    REQUIRE(text.find("line3") != std::string::npos);
    REQUIRE(text.find("line4") != std::string::npos);
    REQUIRE(text.find("line0") == std::string::npos);
    REQUIRE(text.find("line1") == std::string::npos);
    REQUIRE(text.find("line2") == std::string::npos);
}

TEST_CASE("file viewer shows scroll hint", "[file_viewer][render]") {
    FileViewState f = make_file({"a"});
    const auto text = render_elem(build_file_viewer(f));
    REQUIRE(text.find("Esc 关闭") != std::string::npos);
}

TEST_CASE("file viewer line numbers right aligned", "[file_viewer][render]") {
    // 10 行 → 行号 2 位：1-9 前补空格，10 不补
    std::vector<std::string> lines;
    for (int i = 0; i < 10; ++i) lines.push_back("x" + std::to_string(i));
    FileViewState f = make_file(std::move(lines), "f.txt", "", 0);
    const auto text = render_elem(build_file_viewer(f), 40, 24);
    // 行号 10 存在且紧邻内容（无前导空格），行号 1 有前导空格
    REQUIRE(text.find("10 x9") != std::string::npos);
    REQUIRE(text.find(" 1 x0") != std::string::npos);
}

TEST_CASE("file viewer lang_from_path infers language", "[file_viewer][lang]") {
    REQUIRE(lang_from_path("src/main.cpp") == "cpp");
    REQUIRE(lang_from_path("src/main.py") == "python");
    REQUIRE(lang_from_path("CMakeLists.txt") == "cpp");
    REQUIRE(lang_from_path("Dockerfile") == "bash");
    REQUIRE(lang_from_path("README.md") == "");
    REQUIRE(lang_from_path("noext") == "");
    REQUIRE(lang_from_path("") == "");
}

// ============================================================================
// 内联 diff 高亮（P4）
// ============================================================================

TEST_CASE("file viewer marks inserted line with + marker", "[file_viewer][diff]") {
    FileViewState f = make_file({"int main() {", "  auto y = bar();", "  return 0;", "}"});
    // 修改区块：第 2 行为新增（new_start=2，diff 第 1 行 Insert）
    FileChange ch;
    ch.file_path = "src/main.cpp";
    ch.new_start = 2;
    ch.diff.push_back({agent::DiffKind::Insert, "  auto y = bar();", 1});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    // 第 2 行出现 + 标记；未变更行无 +
    REQUIRE(text.find("+2") != std::string::npos);
    REQUIRE(text.find("+1") == std::string::npos);
    REQUIRE(text.find("+3") == std::string::npos);
}

TEST_CASE("file viewer marks modified line with + marker", "[file_viewer][diff]") {
    FileViewState f = make_file({"int main() {", "  auto y = bar();", "  return 0;", "}"});
    // 修改区块：第 2 行为 Modify（new_start=2）
    FileChange ch;
    ch.file_path = "src/main.cpp";
    ch.new_start = 2;
    ch.diff.push_back({agent::DiffKind::Modify, "  auto y = bar();", 1});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("+2") != std::string::npos);
}

TEST_CASE("file viewer diff with multi-line block marks each changed line", "[file_viewer][diff]") {
    FileViewState f = make_file({"a", "b", "c", "d"});
    FileChange ch;
    ch.file_path = "f.txt";
    ch.new_start = 2;
    ch.diff.push_back({agent::DiffKind::Equal, "b", 1});
    ch.diff.push_back({agent::DiffKind::Insert, "b1", 2});
    ch.diff.push_back({agent::DiffKind::Modify, "c1", 3});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    // 第 2 行 Equal 无 +，第 3 行 Insert 有 +，第 4 行 Modify 有 +
    REQUIRE(text.find("+2") == std::string::npos);
    REQUIRE(text.find("+3") != std::string::npos);
    REQUIRE(text.find("+4") != std::string::npos);
}

TEST_CASE("file viewer ignores change without new_start", "[file_viewer][diff]") {
    FileViewState f = make_file({"a", "b"});
    FileChange ch;
    ch.file_path = "f.txt";
    ch.new_start = 0;  // 未定位（找不到区块）
    ch.diff.push_back({agent::DiffKind::Insert, "b", 1});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    REQUIRE(text.find("+") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 图片预览（/view 及项目树点击图片）
// ---------------------------------------------------------------------------

namespace {

/// @brief 构造纯色/逐像素可控的 RGBA 测试图
std::shared_ptr<ImageData> make_image(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    auto img = std::make_shared<ImageData>();
    img->width = w;
    img->height = h;
    img->rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);
    for (int i = 0; i < w * h; ++i) {
        img->rgba[static_cast<std::size_t>(i) * 4 + 0] = r;
        img->rgba[static_cast<std::size_t>(i) * 4 + 1] = g;
        img->rgba[static_cast<std::size_t>(i) * 4 + 2] = b;
        img->rgba[static_cast<std::size_t>(i) * 4 + 3] = 255;
    }
    return img;
}

}  // namespace

TEST_CASE("is_image_file detects common image extensions", "[file_viewer][image]") {
    REQUIRE(is_image_file("logo.png"));
    REQUIRE(is_image_file("a/b/c.PNG"));  // 大小写不敏感
    REQUIRE(is_image_file("photo.jpeg"));
    REQUIRE(is_image_file("photo.jpg"));
    REQUIRE(is_image_file("img.webp"));
    REQUIRE(is_image_file("icon.bmp"));
    REQUIRE(is_image_file("anim.gif"));
    REQUIRE(is_image_file("tex.tga"));
    REQUIRE_FALSE(is_image_file("main.cpp"));
    REQUIRE_FALSE(is_image_file("readme.md"));
    REQUIRE_FALSE(is_image_file("noext"));
    REQUIRE_FALSE(is_image_file("dir.with.dot/name"));
}

TEST_CASE("file viewer image mode shows path and size meta", "[file_viewer][render][image]") {
    FileViewState f;
    f.path = "assets/logo.png";
    f.image = make_image(64, 32, 255, 0, 0);

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("assets/logo.png") != std::string::npos);
    REQUIRE(text.find("64x32") != std::string::npos);
    REQUIRE(text.find("图片预览") != std::string::npos);
}

TEST_CASE("image content renders half-block truecolor cells", "[file_viewer][render][image]") {
    // 2x2 图像：左上红 / 右上绿 / 左下蓝 / 右下白（半块每终端单元格 2 像素）
    auto img = std::make_shared<ImageData>();
    img->width = 2;
    img->height = 2;
    img->rgba = {255, 0, 0,   255, 0,   255, 0,   255,  //
                 0,   0, 255, 255, 255, 255, 255, 255};

    auto e = build_image_content(*img, 40, 24, 0);
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(40), ftxui::Dimension::Fixed(24));
    ftxui::Render(screen, e);

    // 收集半块 cell（应恰好 2 个，水平居中于 40 宽：x=19/x=20）
    struct Seen {
        int x;
        ftxui::Color fg;
        ftxui::Color bg;
    };
    std::vector<Seen> seen;
    for (int y = 0; y < 24 && seen.size() < 3; ++y)
        for (int x = 0; x < 40; ++x) {
            const auto& cell = screen.PixelAt(x, y);
            if (cell.character == "\u2580")
                seen.push_back({x, cell.foreground_color, cell.background_color});
        }
    REQUIRE(seen.size() == 2);
    // 左半块：上=红(左上) 下=蓝(左下)
    REQUIRE(seen[0].fg == ftxui::Color::RGB(255, 0, 0));
    REQUIRE(seen[0].bg == ftxui::Color::RGB(0, 0, 255));
    // 右半块：上=绿(右上) 下=白(右下)
    REQUIRE(seen[1].fg == ftxui::Color::RGB(0, 255, 0));
    REQUIRE(seen[1].bg == ftxui::Color::RGB(255, 255, 255));
}

TEST_CASE("image_view_rows fits aspect ratio without upscale", "[file_viewer][image]") {
    // 400x200 在 40 列视口：宽比 0.1 → 垂直 200*0.1=20 像素 = 10 半块行
    ImageData img;
    img.width = 400;
    img.height = 200;
    img.rgba.resize(static_cast<std::size_t>(400) * 200 * 4, 0);
    REQUIRE(image_view_rows(img, 40, 10) == 10);

    // 小图不放大：4x2 → 视口 40x24，scale 钳到 1 → 保持原尺寸 → ceil(2/2)=1 行
    ImageData small;
    small.width = 4;
    small.height = 2;
    small.rgba.resize(static_cast<std::size_t>(4) * 2 * 4, 0);
    REQUIRE(image_view_rows(small, 40, 24) == 1);

    // 2x4 → 保持原尺寸 → ceil(4/2)=2 行
    ImageData tall;
    tall.width = 2;
    tall.height = 4;
    tall.rgba.resize(static_cast<std::size_t>(2) * 4 * 4, 0);
    REQUIRE(image_view_rows(tall, 40, 24) == 2);
}

TEST_CASE("decode_image_file decodes real PNG from repo", "[file_viewer][image]") {
    const auto png =
        (std::filesystem::path(SOURCE_DIR) / "docs" / "img" / "11_module_dependency.png").string();
    std::string err;
    auto img = decode_image_file(png, &err);
    REQUIRE(img != nullptr);
    REQUIRE(img->width > 0);
    REQUIRE(img->height > 0);
    REQUIRE(img->rgba.size() == static_cast<std::size_t>(img->width) * img->height * 4);
}

TEST_CASE("decode_image_file reports error for missing file", "[file_viewer][image]") {
    std::string err;
    auto img = decode_image_file("no_such_file.png", &err);
    REQUIRE(img == nullptr);
    REQUIRE_FALSE(err.empty());
}
