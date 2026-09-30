/**
 * @file test_provider_manager.cpp
 * @brief 供应商管理面板交互无头单元测试
 * @details 覆盖列表层 Enter 激活、↑↓ 选择、面板关闭、on_activate 回调入参。
 *          直接对 ProviderManager 组件调用 OnEvent（无渲染/无屏幕）。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <ftxui/component/event.hpp>

#include "agent/model/provider_config.h"
#include "widgets/provider_manager.h"

using namespace ftxtui;
using agent::ProviderConfigEntry;

namespace {

/// 构造 3 个供应商的面板，返回组件与 on_activate 记录
struct Fixture {
    std::vector<ProviderConfigEntry> providers;
    std::string active_id;
    bool open = false;
    std::vector<int> activated;
    ftxui::Component comp;

    Fixture() {
        auto mk = [](std::string id, std::string name, std::string url, std::string model) {
            ProviderConfigEntry e;
            e.id = std::move(id);
            e.name = std::move(name);
            e.base_url = std::move(url);
            e.model = std::move(model);
            return e;
        };
        providers.push_back(mk("deepseek", "DeepSeek", "https://api.deepseek.com", "ds-v3"));
        providers.push_back(mk("moma", "移动MOMA", "https://zhenze.cmecloud.cn", "v4"));
        providers.push_back(mk("lm", "LmStudio", "http://127.0.0.1:1234", "gemma"));
        active_id = "lm";  // 当前使用中 → 打开后定位第 3 项
        open = true;
        comp = make_provider_manager(
            ProviderManagerOptions{
                .providers = providers,
                .active_id = active_id,
                .catalog = nullptr,
                .on_activate = [this](int idx) { activated.push_back(idx); },
                .on_commit = nullptr,
                .on_close = nullptr,
                .title = "供应商管理",
            },
            open);
    }
};

}  // namespace

TEST_CASE("provider list activates current selection on Enter", "[provider]") {
    Fixture f;
    REQUIRE(f.open);
    // 打开定位到当前使用中（lm，下标 2）
    f.comp->Render();
    REQUIRE(f.activated.empty());
    // 向上移一项 → 下标 1（移动MOMA）
    REQUIRE(f.comp->OnEvent(ftxui::Event::ArrowUp));
    // Enter 激活
    REQUIRE(f.comp->OnEvent(ftxui::Event::Return));
    REQUIRE(f.activated.size() == 1);
    REQUIRE(f.activated[0] == 1);
    // 面板应关闭
    REQUIRE_FALSE(f.open);
}

TEST_CASE("provider list wraps selection around ends", "[provider]") {
    Fixture f;
    f.comp->Render();
    // 从下标 2 向下 → 3 → 环绕到 0
    REQUIRE(f.comp->OnEvent(ftxui::Event::ArrowDown));
    REQUIRE(f.comp->OnEvent(ftxui::Event::Return));
    REQUIRE(f.activated.size() == 1);
    REQUIRE(f.activated[0] == 0);
}

TEST_CASE("provider list activates on Tab and Space", "[provider]") {
    Fixture f;
    f.comp->Render();
    REQUIRE(f.comp->OnEvent(ftxui::Event::ArrowUp));  // 2 → 1
    REQUIRE(f.comp->OnEvent(ftxui::Event::Tab));
    REQUIRE(f.activated.size() == 1);
    REQUIRE(f.activated[0] == 1);
    REQUIRE_FALSE(f.open);
}

TEST_CASE("provider list does not activate on unrelated keys", "[provider]") {
    Fixture f;
    f.comp->Render();
    // 非激活键（字符 e 编辑、a 添加）不得触发 on_activate
    REQUIRE(f.comp->OnEvent(ftxui::Event::Character("e")));
    REQUIRE(f.comp->OnEvent(ftxui::Event::Character("a")));
    REQUIRE(f.activated.empty());
    REQUIRE(f.open);
}

TEST_CASE("provider Escape closes without activating", "[provider]") {
    Fixture f;
    f.comp->Render();
    REQUIRE(f.comp->OnEvent(ftxui::Event::Escape));
    REQUIRE_FALSE(f.open);
    REQUIRE(f.activated.empty());
}
