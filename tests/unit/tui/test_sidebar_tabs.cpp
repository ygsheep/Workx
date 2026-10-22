/**
 * @file test_sidebar_tabs.cpp
 * @brief 侧边栏 tab 容器无头渲染测试
 * @details 覆盖：默认任务调度 tab、可开合 tab（变更记录/文件）显隐、
 *          ✕ 关闭按钮显隐、tab 命中区记录（切换/关闭区分）、
 *          子 Agent 卡片（一行两个）、MCP/TODO 可折叠区块、上下文信息。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>

#include <ftxui/screen/screen.hpp>

#include "core/todo/todo_item.h"
#include "widgets/sidebar_tabs.h"

using namespace ftxtui;

namespace {

/// @brief 构造待办条目（#24：SidebarModel.todos 升级为结构化类型）
core::todo::TodoItem make_todo(const std::string& content) {
    core::todo::TodoItem item;
    item.content = content;
    return item;
}

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

SidebarTabsModel make_tabs() {
    SidebarTabsModel t;
    t.active = SidebarTab::kTasks;
    return t;
}

}  // namespace

// ============================================================================
// 默认状态：任务调度 tab 常驻，变更记录/文件隐藏
// ============================================================================

TEST_CASE("sidebar tabs default shows tasks tab only", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("任务调度") != std::string::npos);
    REQUIRE(text.find("变更记录") == std::string::npos);  // 未打开不显示
    REQUIRE(text.find("文件") == std::string::npos);      // 未打开不显示
}

TEST_CASE("sidebar tabs tasks content shows aggregation sections", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.sub_agents.push_back(SubAgentLite{.task_id = "t1", .status = "running"});
    tabs.background_tasks.push_back(TaskLite{.name = "bg1", .status = "Running"});
    SidebarModel sidebar;
    sidebar.model = "gpt-test";
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("当前工具") != std::string::npos);
    REQUIRE(text.find("步骤") != std::string::npos);
    REQUIRE(text.find("子 Agent") != std::string::npos);
    REQUIRE(text.find("后台任务") != std::string::npos);
    REQUIRE(text.find("gpt-test") != std::string::npos);
}

TEST_CASE("sidebar tabs keeps original sidebar info", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.project = "workx";
    sidebar.model = "m1";
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("workx") != std::string::npos);
    REQUIRE(text.find("m1") != std::string::npos);
}

// ============================================================================
// 上下文信息（statusbar 风格）：token + 上下文使用大小 + 缓存
// ============================================================================

TEST_CASE("sidebar tabs context info shows token and cache", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.context_limit = 200000;
    sidebar.context_used = 12000;
    sidebar.cache_read_tokens = 8000;
    sidebar.total_tokens = 12000;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("上下文") != std::string::npos);
    REQUIRE(text.find("Token") != std::string::npos);
    REQUIRE(text.find("cache") != std::string::npos);  // 缓存命中显示
    REQUIRE(text.find("12k") != std::string::npos);    // 12k/200k
    REQUIRE(text.find("200k") != std::string::npos);
}

TEST_CASE("sidebar tabs context info hides cache when zero", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.context_limit = 100000;
    sidebar.context_used = 5000;
    sidebar.cache_read_tokens = 0;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("cache") == std::string::npos);
}

TEST_CASE("sidebar tabs removes cost", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.cost_usd = 1.23;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("成本") == std::string::npos);  // 成本已移除
}

TEST_CASE("sidebar tabs shows DS cache hit rate", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.cache_hit_tokens = 8000;
    sidebar.cache_miss_tokens = 2000;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("DS 缓存") != std::string::npos);
    REQUIRE(text.find("命中") != std::string::npos);
    REQUIRE(text.find("80%") != std::string::npos);  // 8000/10000
    REQUIRE(text.find("8k/10k") != std::string::npos);
}

TEST_CASE("sidebar tabs hides hit rate when no cache data", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("命中") == std::string::npos);
}

TEST_CASE("sidebar tabs shows prompt and generated breakdown", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.prompt_tokens = 12000;
    sidebar.generated_tokens = 2000;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("Prompt") != std::string::npos);
    REQUIRE(text.find("生成") != std::string::npos);
    REQUIRE(text.find("12k") != std::string::npos);
    REQUIRE(text.find("2k") != std::string::npos);
}

TEST_CASE("sidebar tabs hides breakdown when all zero", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("Prompt") == std::string::npos);
    REQUIRE(text.find("生成") == std::string::npos);
}

// ============================================================================
// 子 Agent 菜单：单行标签 + 菜单元素嵌入
// ============================================================================

TEST_CASE("sidebar tabs sub agent label formats running step", "[sidebar_tabs][render]") {
    SubAgentLite a{.task_id = "task_1", .status = "running", .step_number = 3};
    const std::string label = sub_agent_label(a);
    REQUIRE(label.find("●") != std::string::npos);
    REQUIRE(label.find("task_1") != std::string::npos);
    REQUIRE(label.find("步骤 3") != std::string::npos);
}

TEST_CASE("sidebar tabs sub agent label formats done and failed", "[sidebar_tabs][render]") {
    SubAgentLite done{.task_id = "task_1", .status = "done"};
    REQUIRE(sub_agent_label(done).find("完成") != std::string::npos);
    SubAgentLite failed{.task_id = "task_2", .status = "failed"};
    REQUIRE(sub_agent_label(failed).find("失败") != std::string::npos);
}

TEST_CASE("sidebar tabs embeds sub agent menu element in tasks tab", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.sub_agents.push_back(SubAgentLite{.task_id = "task_1", .status = "running"});
    const auto text = render_elem(
        build_sidebar_tabs(tabs, SidebarModel{}, nullptr, nullptr, ftxui::text("MENU_ENTRY")));
    REQUIRE(text.find("子 Agent") != std::string::npos);
    REQUIRE(text.find("MENU_ENTRY") != std::string::npos);
}

// ============================================================================
// MCP / TODO 可折叠区块
// ============================================================================

TEST_CASE("sidebar tabs collapsible sections expanded by default", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.mcp_servers = {{"server-a", "2026-07-28", 2, 1, ""}};
    sidebar.todos = {make_todo("todo-1")};
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("MCP") != std::string::npos);
    REQUIRE(text.find("TODO") != std::string::npos);
    REQUIRE(text.find("server-a") != std::string::npos);  // 展开显示条目
    REQUIRE(text.find("todo-1") != std::string::npos);
}

TEST_CASE("sidebar tabs collapsible sections hide items when collapsed", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.mcp_expanded = false;
    sidebar.todo_expanded = false;
    sidebar.mcp_servers = {{"server-a", "2026-07-28", 2, 1, ""}};
    sidebar.todos = {make_todo("todo-1")};
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("MCP") != std::string::npos);  // 标题仍在
    REQUIRE(text.find("TODO") != std::string::npos);
    REQUIRE(text.find("server-a") == std::string::npos);  // 条目隐藏
    REQUIRE(text.find("todo-1") == std::string::npos);
}

TEST_CASE("sidebar tabs mcp failed server shows error text", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    SidebarModel sidebar;
    sidebar.mcp_servers = {{"broken", "", 0, 2, "spawn failed"}};
    const auto text = render_elem(build_sidebar_tabs(tabs, sidebar));
    REQUIRE(text.find("broken") != std::string::npos);
    REQUIRE(text.find("spawn failed") != std::string::npos);  // 失败错误信息可见
}

TEST_CASE("sidebar tabs records section hit boxes", "[sidebar_tabs][hit]") {
    SidebarTabsModel tabs = make_tabs();
    std::deque<TabHit> tab_hits;
    std::deque<SectionHit> section_hits;
    render_elem(build_sidebar_tabs(tabs, SidebarModel{}, &tab_hits, &section_hits));
    REQUIRE(section_hits.size() == 2);  // MCP + TODO
    REQUIRE(section_hits[0].kind == SectionHit::Kind::kMCP);
    REQUIRE(section_hits[1].kind == SectionHit::Kind::kTODO);
    for (const auto& h : section_hits) {
        REQUIRE(h.box.x_min >= 0);
        REQUIRE(h.box.x_max >= h.box.x_min);
        REQUIRE(h.box.y_min >= 0);
        REQUIRE(h.box.y_max >= h.box.y_min);
    }
}

// ============================================================================
// 可开合 tab：打开后显示且带 ✕，关闭后消失
// ============================================================================

TEST_CASE("sidebar tabs changes tab appears with close button when open",
          "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.changes_open = true;
    const auto text = render_elem(build_sidebar_tabs(tabs, SidebarModel{}), 40, 24);
    REQUIRE(text.find("变更记录") != std::string::npos);
    REQUIRE(text.find("✕") != std::string::npos);
}

TEST_CASE("sidebar tabs file tab appears with close button when open", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.file_open = true;
    const auto text = render_elem(build_sidebar_tabs(tabs, SidebarModel{}));
    REQUIRE(text.find("文件") != std::string::npos);
    REQUIRE(text.find("✕") != std::string::npos);
}

TEST_CASE("sidebar tabs tasks tab has no close button", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    const auto text = render_elem(build_sidebar_tabs(tabs, SidebarModel{}));
    // 任务调度常驻无 ✕；变更记录/文件未打开，整体不应出现 ✕
    REQUIRE(text.find("✕") == std::string::npos);
}

TEST_CASE("sidebar tabs selected tab shows border line", "[sidebar_tabs][render]") {
    // 选中 tab（默认任务调度）用白色边框线；未选中 tab 无边框线
    SidebarTabsModel tabs = make_tabs();  // active = kTasks（选中）
    tabs.changes_open = true;             // 变更记录未选中 → 无边框
    const auto text = render_elem(build_sidebar_tabs(tabs, SidebarModel{}), 40, 24);
    INFO("RENDERED:\n" << text);
    // 选中 tab 的边框线（┌ ┐ └ ┘ 仅出现在选中 tab 上）
    REQUIRE(text.find("┌") != std::string::npos);
    REQUIRE(text.find("┐") != std::string::npos);
    REQUIRE(text.find("└") != std::string::npos);
    REQUIRE(text.find("┘") != std::string::npos);
}

TEST_CASE("sidebar tabs unselected tab has no border line", "[sidebar_tabs][render]") {
    // 变更记录 tab 打开但未选中 → 无边框线（borderEmpty 占位）
    SidebarTabsModel tabs = make_tabs();
    tabs.changes_open = true;
    const auto text = render_elem(build_sidebar_tabs(tabs, SidebarModel{}), 40, 24);
    // 仅任务调度选中 → 边框线只围绕任务调度；变更记录无独立边框
    // 边框字符总数：选中 tab 一个边框 = 1 组 ┌┐└┘
    REQUIRE(text.find("┌") != std::string::npos);
    REQUIRE(text.find("└") != std::string::npos);
}

TEST_CASE("sidebar tabs empty shells show placeholders", "[sidebar_tabs][render]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.changes_open = true;
    tabs.active = SidebarTab::kChanges;
    const auto text = render_elem(build_sidebar_tabs(tabs, SidebarModel{}));
    REQUIRE(text.find("暂无文件修改") != std::string::npos);

    SidebarTabsModel tabs2 = make_tabs();
    tabs2.file_open = true;
    tabs2.active = SidebarTab::kFiles;
    const auto text2 = render_elem(build_sidebar_tabs(tabs2, SidebarModel{}));
    REQUIRE(text2.find("暂无打开的文件") != std::string::npos);
}

// ============================================================================
// 命中区：切换区与 ✕ 关闭区分别记录
// ============================================================================

TEST_CASE("sidebar tabs records hit boxes for visible tabs", "[sidebar_tabs][hit]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.changes_open = true;
    tabs.file_open = true;
    std::deque<TabHit> hits;
    // 4 个 CJK tab + 边框 + 关闭按钮总宽较大，需足够宽度避免末 tab box 反转
    render_elem(build_sidebar_tabs(tabs, SidebarModel{}, &hits), 60, 24);

    // 4 个可见 tab（任务调度/项目/变更记录/文件）：每个 1 个切换区 +
    // 2 个可关闭 tab 各 1 个关闭区 = 6
    REQUIRE(hits.size() == 6);

    int tasks_switch = 0, changes_switch = 0, files_switch = 0;
    int changes_close = 0, files_close = 0;
    for (const auto& h : hits) {
        if (h.close) {
            if (h.tab == SidebarTab::kChanges) ++changes_close;
            if (h.tab == SidebarTab::kFiles) ++files_close;
        } else {
            if (h.tab == SidebarTab::kTasks) ++tasks_switch;
            if (h.tab == SidebarTab::kChanges) ++changes_switch;
            if (h.tab == SidebarTab::kFiles) ++files_switch;
        }
        // 命中区必须落在渲染区域内
        REQUIRE(h.box.x_min >= 0);
        REQUIRE(h.box.x_max >= h.box.x_min);
        REQUIRE(h.box.y_min >= 0);
        REQUIRE(h.box.y_max >= h.box.y_min);
    }
    REQUIRE(tasks_switch == 1);
    REQUIRE(changes_switch == 1);
    REQUIRE(files_switch == 1);
    REQUIRE(changes_close == 1);
    REQUIRE(files_close == 1);
}

TEST_CASE("sidebar tabs no hit boxes when only resident tabs", "[sidebar_tabs][hit]") {
    SidebarTabsModel tabs = make_tabs();
    std::deque<TabHit> hits;
    render_elem(build_sidebar_tabs(tabs, SidebarModel{}, &hits));
    // 常驻 2 tab（任务调度/项目）：各 1 个切换区；可关闭 tab 未打开无关闭区
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].tab == SidebarTab::kTasks);
    REQUIRE(!hits[0].close);
}

TEST_CASE("sidebar tabs close hit is inside switch hit", "[sidebar_tabs][hit]") {
    SidebarTabsModel tabs = make_tabs();
    tabs.changes_open = true;
    std::deque<TabHit> hits;
    render_elem(build_sidebar_tabs(tabs, SidebarModel{}, &hits));

    // 变更记录：切换区（close=false）与关闭区（close=true）同 tab，关闭区应包含于切换区
    const TabHit* sw = nullptr;
    const TabHit* cl = nullptr;
    for (const auto& h : hits) {
        if (h.tab == SidebarTab::kChanges && !h.close) sw = &h;
        if (h.tab == SidebarTab::kChanges && h.close) cl = &h;
    }
    REQUIRE(sw != nullptr);
    REQUIRE(cl != nullptr);
    REQUIRE(cl->box.x_min >= sw->box.x_min);
    REQUIRE(cl->box.x_max <= sw->box.x_max);
    REQUIRE(cl->box.y_min >= sw->box.y_min);
    REQUIRE(cl->box.y_max <= sw->box.y_max);
}
