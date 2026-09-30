/**
 * @file test_suggest_logic.cpp
 * @brief 输入栏提示面板 / 聚合搜索面板无头逻辑单元测试
 * @details 覆盖："/" 与 "@" 前缀模式推导（对齐 bottom_bar 语义）、
 *          命令过滤（大小写不敏感 + 多字段匹配 + 保持顺序）、
 *          聚合搜索过滤（空查询全量、标题前缀命中优先、类别标签齐全）。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

#include "widgets/search_palette.h"
#include "widgets/suggest_panel.h"

using namespace ftxtui;

namespace {

/// @brief 把组件渲染到固定尺寸 Screen（面板默认去噪行为验证用）
std::string render_comp(const ftxui::Component& c, int cols = 80, int rows = 20) {
    auto screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(cols), ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, c->Render());
    std::string out;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) out += screen.PixelAt(x, y).character;
        out += '\n';
    }
    return out;
}

}  // namespace

// ============================================================================
// parse_suggest_query："/" 命令模式
// ============================================================================

TEST_CASE("parse_suggest_query leading slash enters command mode", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("/mod", q) == SuggestMode::Command);
    REQUIRE(q == "mod");
}

TEST_CASE("parse_suggest_query bare slash gives empty query", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("/", q) == SuggestMode::Command);
    REQUIRE(q.empty());
}

TEST_CASE("parse_suggest_query no mode without any slash", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("hello", q) == SuggestMode::None);
}

TEST_CASE("parse_suggest_query inline slash enters command mode", "[suggest][parse]") {
    std::string q;
    // "aaa/"：行内最后一个 "/"（无需行首）进入命令模式，空查询列出全部命令
    REQUIRE(parse_suggest_query("aaa/", q) == SuggestMode::Command);
    REQUIRE(q.empty());
    REQUIRE(parse_suggest_query("aaa/model", q) == SuggestMode::Command);
    REQUIRE(q == "model");
}

// ============================================================================
// parse_suggest_query："@" 文件模式（行内最后一个 @ 且其后无空格）
// ============================================================================

TEST_CASE("parse_suggest_query inline at enters file mode", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("看看 @src", q) == SuggestMode::File);
    REQUIRE(q == "src");
}

TEST_CASE("parse_suggest_query leading at is file mode too", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("@src/core", q) == SuggestMode::File);
    REQUIRE(q == "src/core");
}

TEST_CASE("parse_suggest_query at followed by space exits file mode", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("聊聊 @ src", q) == SuggestMode::None);
}

TEST_CASE("parse_suggest_query last at wins as query start", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("a @b 看 @c", q) == SuggestMode::File);
    REQUIRE(q == "c");
}

TEST_CASE("parse_suggest_query trailing at wins over slash", "[suggest][parse]") {
    std::string q;
    // 命令路径含空格 → 命令模式放弃，最新 "@" 进入文件模式
    REQUIRE(parse_suggest_query("/mod @x", q) == SuggestMode::File);
    REQUIRE(q == "x");
}

TEST_CASE("parse_suggest_query command arg at enters file mode", "[suggest][parse]") {
    std::string q;
    // "/view @path"：命令参数用 @ 触发文件搜索面板，优先文件模式
    REQUIRE(parse_suggest_query("/view @", q) == SuggestMode::File);
    REQUIRE(q.empty());
    REQUIRE(parse_suggest_query("/view @src", q) == SuggestMode::File);
    REQUIRE(q == "src");
    REQUIRE(parse_suggest_query("/nvim @src/core/main.cpp", q) == SuggestMode::File);
    REQUIRE(q == "src/core/main.cpp");
    // 命令多词（含空格）+ @ 同样走文件模式
    REQUIRE(parse_suggest_query("/skill-001 看 @app", q) == SuggestMode::File);
    REQUIRE(q == "app");
}

TEST_CASE("parse_suggest_query command arg without at stays command mode", "[suggest][parse]") {
    std::string q;
    // 命令参数不带 @：仍为命令模式（如 /view、/nvim 自身补全）
    REQUIRE(parse_suggest_query("/view", q) == SuggestMode::Command);
    REQUIRE(q == "view");
    REQUIRE(parse_suggest_query("/nvim x", q) == SuggestMode::None);
}

// ============================================================================
// filter_commands：子串过滤
// ============================================================================

TEST_CASE("filter_commands empty query returns all", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model", "/help"};
    REQUIRE(filter_commands(cmds, "").size() == 3);
}

TEST_CASE("filter_commands is case insensitive", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    auto hits = filter_commands(cmds, "CLEAR");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);
}

TEST_CASE("filter_commands matches substring not prefix", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    auto hits = filter_commands(cmds, "le");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);  // "/clear" 中间子串 "le"，非前缀
}

TEST_CASE("filter_commands no match returns empty", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    REQUIRE(filter_commands(cmds, "zzz").empty());
}

TEST_CASE("filter_commands keeps source order", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    auto hits = filter_commands(cmds, "e");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 0);
    REQUIRE(hits[1] == 1);
}

// ============================================================================
// apply_command_suggest：命令面板确认追加（保留 "/" 之前已输入内容）
// ============================================================================

TEST_CASE("apply_command_suggest keeps prefix before slash", "[suggest][accept]") {
    // "test /skill" + Enter 选中 "/skill" → 追加为 "test /skill "（不丢 "test "）
    REQUIRE(apply_command_suggest("test /skill", "/skill") == "test /skill ");
    REQUIRE(apply_command_suggest("test /skill", "/skill-001") == "test /skill-001 ");
    REQUIRE(apply_command_suggest("test /skill", "/new") == "test /new ");
}

TEST_CASE("apply_command_suggest leading slash replaces tail", "[suggest][accept]") {
    REQUIRE(apply_command_suggest("/mod", "/model") == "/model ");
    REQUIRE(apply_command_suggest("/", "/help") == "/help ");
}

TEST_CASE("apply_command_suggest empty line appends", "[suggest][accept]") {
    REQUIRE(apply_command_suggest("", "/help") == "/help ");
}

TEST_CASE("apply_command_suggest slash inside at path appends", "[suggest][accept]") {
    // "@src/core" 中的 "/" 是文件路径分隔符，不当作命令触发符 → 直接追加
    REQUIRE(apply_command_suggest("看 @src/core", "/clear") == "看 @src/core/clear ");
}

TEST_CASE("apply_command_suggest supports multi command chain", "[suggest][accept]") {
    // 连续追加：输入 "/skill-001 /skill002"（末尾无空格触发命令面板）后
    // Enter 选中 "/skill002" → "/skill-001 /skill002 "
    REQUIRE(apply_command_suggest("/skill-001 /skill002", "/skill002") == "/skill-001 /skill002 ");
}

// ============================================================================
// filter_search_entries：聚合搜索过滤
// ============================================================================

static SearchEntry make_entry(SearchCategory c, std::string title) {
    SearchEntry e;
    e.category = c;
    e.title = std::move(title);
    return e;
}

TEST_CASE("filter_search_entries empty query returns all", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
        make_entry(SearchCategory::File, "app.cpp"),
        make_entry(SearchCategory::Session, "昨天的工作"),
        make_entry(SearchCategory::Setting, "自动滚动"),
    };
    REQUIRE(filter_search_entries(entries, "").size() == 4);
}

TEST_CASE("filter_search_entries title prefix hits rank first", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "新会话"),
        make_entry(SearchCategory::Setting, "会话列表上限"),
    };
    // "会话" 命中第二条的标题前缀，应排到第一条前
    auto hits = filter_search_entries(entries, "会话");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 1);
    REQUIRE(hits[1] == 0);
}

TEST_CASE("filter_search_entries keywords field participates", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "清空对话"),
    };
    entries[0].keywords = "clear";
    auto hits = filter_search_entries(entries, "clear");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);
}

TEST_CASE("filter_search_entries mode selector entries match by title/keywords",
          "[palette][filter][mode]") {
    // 模式选择面板条目（标准/计划/极简 + 介绍副标题）
    std::vector<SearchEntry> entries;
    auto push = [&](std::string title, std::string desc, std::string kw, bool active) {
        SearchEntry e;
        e.category = SearchCategory::Setting;
        e.title = std::move(title);
        e.subtitle = std::move(desc);
        e.keywords = std::move(kw);
        e.active = active;
        entries.push_back(std::move(e));
    };
    push("标准模式", "全部工具可用，权限独立切换", "standard", true);
    push("计划模式", "只读规划：禁止写文件与执行命令", "plan", false);
    push("极简模式", "仅支持 Skill / Bash / Read / Write / Edit", "minimal", false);

    // 空查询：全部命中
    REQUIRE(filter_search_entries(entries, "").size() == 3);
    // 按标题中文命中
    auto zh = filter_search_entries(entries, "极简");
    REQUIRE(zh.size() == 1);
    REQUIRE(zh[0] == 2);
    // 按关键字（英文标签）命中
    auto en = filter_search_entries(entries, "plan");
    REQUIRE(en.size() == 1);
    REQUIRE(en[0] == 1);
    // 副标题（模式介绍）参与搜索
    auto desc = filter_search_entries(entries, "只读");
    REQUIRE(desc.size() == 1);
    REQUIRE(desc[0] == 1);
}

TEST_CASE("filter_search_entries is case insensitive", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::File, "src/core/utils.cpp"),
    };
    REQUIRE(filter_search_entries(entries, "CORE").size() == 1);
    REQUIRE(filter_search_entries(entries, "UTILS").size() == 1);
}

TEST_CASE("filter_search_entries no match returns empty", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
    };
    REQUIRE(filter_search_entries(entries, "不存在的内容").empty());
}

TEST_CASE("filter_search_entries keeps source order for substring hits", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Session, "第一场"),
        make_entry(SearchCategory::Feature, "第二个"),
        make_entry(SearchCategory::File, "第三场"),
    };
    auto hits = filter_search_entries(entries, "场");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 0);
    REQUIRE(hits[1] == 2);
}

TEST_CASE("filter_search_entries slash prefix restricts to Feature", "[palette][filter][prefix]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型 /model"),
        make_entry(SearchCategory::File, "src/model.cpp"),
        make_entry(SearchCategory::Session, "昨天模型讨论"),
    };
    // "/model" 去掉前缀后只搜功能类，文件/会话被排除
    auto hits = filter_search_entries(entries, "/model");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);
}

TEST_CASE("filter_search_entries slash prefix no match empty", "[palette][filter][prefix]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "清空会话"),
        make_entry(SearchCategory::File, "app.cpp"),
    };
    REQUIRE(filter_search_entries(entries, "/app").empty());  // 功能类无 app
}

TEST_CASE("filter_search_entries at prefix restricts to File", "[palette][filter][prefix]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "app路径"),
        make_entry(SearchCategory::File, "src/app/main.cpp"),
        make_entry(SearchCategory::Session, "app会话"),
    };
    auto hits = filter_search_entries(entries, "@app");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 1);
}

TEST_CASE("filter_search_entries at prefix no match empty", "[palette][filter][prefix]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::File, "main.cpp"),
        make_entry(SearchCategory::Feature, "切换模型"),
    };
    REQUIRE(filter_search_entries(entries, "@模型").empty());  // 文件类无模型
}

TEST_CASE("filter_search_entries other prefixes unaffected", "[palette][filter][prefix]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "a"),
        make_entry(SearchCategory::File, "b"),
        make_entry(SearchCategory::Session, "c"),
    };
    // 非 @ / 前缀不限定类别
    REQUIRE(filter_search_entries(entries, "b").size() == 1);
    REQUIRE(filter_search_entries(entries, "a").size() == 1);
}

// ============================================================================
// 聚合面板默认去噪：restrict_default=true 且空查询时仅显示「会话记录 / 设置」
// ============================================================================

TEST_CASE("search palette restrict_default hides feature/file on empty query",
          "[palette][filter][default]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
        make_entry(SearchCategory::File, "app.cpp"),
        make_entry(SearchCategory::Session, "昨天的工作"),
        make_entry(SearchCategory::Setting, "自动滚动"),
    };
    bool open = true;
    auto win =
        make_search_palette(entries, [](int) {}, open, nullptr, "", /*restrict_default=*/true);
    const auto text = render_comp(win);
    REQUIRE(text.find("切换模型") == std::string::npos);    // 功能隐藏
    REQUIRE(text.find("app.cpp") == std::string::npos);     // 文件隐藏
    REQUIRE(text.find("昨天的工作") != std::string::npos);  // 会话保留
    REQUIRE(text.find("自动滚动") != std::string::npos);    // 设置保留
}

TEST_CASE("search palette restrict_default lifts on typed query", "[palette][filter][default]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
        make_entry(SearchCategory::File, "app.cpp"),
        make_entry(SearchCategory::Session, "昨天的工作"),
        make_entry(SearchCategory::Setting, "自动滚动"),
    };
    bool open = true;
    auto win =
        make_search_palette(entries, [](int) {}, open, nullptr, "", /*restrict_default=*/true);
    // 空查询默认只显示会话/设置
    REQUIRE(render_comp(win).find("切换模型") == std::string::npos);
    // 输入搜索词后恢复全类搜索，功能条目重新可见
    win->OnEvent(ftxui::Event::Character("切换"));
    REQUIRE(render_comp(win).find("切换模型") != std::string::npos);
}

TEST_CASE("search palette no restrict_default shows all on empty query",
          "[palette][filter][default]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
        make_entry(SearchCategory::File, "app.cpp"),
        make_entry(SearchCategory::Session, "昨天的工作"),
        make_entry(SearchCategory::Setting, "自动滚动"),
    };
    bool open = true;
    auto win =
        make_search_palette(entries, [](int) {}, open, nullptr, "", /*restrict_default=*/false);
    const auto text = render_comp(win);
    REQUIRE(text.find("切换模型") != std::string::npos);  // 默认 false 不影响 /model /resume
    REQUIRE(text.find("app.cpp") != std::string::npos);
}