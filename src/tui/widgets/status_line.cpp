#include "widgets/status_line.h"

#include <format>
#include <string>
#include <string_view>

#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
/// @brief 工作模式展示规格（label：中文标签；color：主题色）
/// @note 模式置于状态行最前，前置转录思考动画（busy 旋转 / 空闲静态点）
struct ModeSpec {
    std::string_view label;
    Color color;
};

/// @brief 工作模式 → 展示规格 映射表（key："standard" / "plan" / "minimal"）
/// @note 不用 constexpr：Color::xxx 非编译期常量
const ModeSpec kModeTable[] = {
    {str::kStatusStandard, theme::T::Accent},    // 标准模式
    {str::kStatusPlan, Color::CyanLight},        // 计划模式
    {str::kStatusMinimal, Color::MagentaLight},  // 极简模式
};

/// @brief 权限展示规格（label：中文标签；color：主题色）
/// @note 计划模式天然只读，状态行隐藏权限位（仅标准/极简展示）
struct PermSpec {
    std::string_view label;
    Color color;
};

const PermSpec kPermTable[] = {
    {str::kStatusManual, Color::GrayLight},        // 手动审批
    {str::kStatusFullAccess, Color::YellowLight},  // 完全访问
};

// 转录思考动画帧：Braille 旋转字符（与 render/markdown_to_elements.cpp 同源）
constexpr const char* kSpinnerFrames[] = {
    "\u2813", "\u2819", "\u2839", "\u2838", "\u283C",
    "\u2834", "\u2826", "\u2827", "\u2807", "\u280F",
};
constexpr int kSpinnerFrameCount = 10;

/// @brief 进行中动画色：HSV 橙黄渐变（与转录思考动画同色相曲线）
Color spinner_color(std::size_t frame) {
    return Color::HSV(static_cast<uint8_t>(20 + (frame * 5) % 26), 255, 255);
}
}  // namespace

Element build_status_line(const std::string& model, const std::string& mode,
                          const std::string& permission, bool busy, std::size_t anim_frame,
                          int todo_done, int todo_total) {
    // 工作模式（标准 / 计划 / 极简），未知/空回退标准
    const ModeSpec mspec = (mode == "plan")      ? kModeTable[1]
                           : (mode == "minimal") ? kModeTable[2]
                                                 : kModeTable[0];

    // 转录思考动画：busy 时旋转帧 + 橙黄渐变；空闲时静态点（弱化）
    const std::string spin = busy ? kSpinnerFrames[anim_frame % kSpinnerFrameCount] : "\u00B7";
    const Color spin_c = busy ? spinner_color(anim_frame) : theme::T::TextFaint;

    Elements seg;
    // 最前：动画 + 模式（替代原"● 生成中 + 权限"组合）
    seg.push_back(ftxui::text(spin) | ftxui::color(spin_c));
    seg.push_back(ftxui::text(" "));
    seg.push_back(ftxui::color(mspec.color)(ftxui::text(std::string(mspec.label))));

    // 权限位：计划模式隐藏（天然只读），标准/极简展示 手动审批 / 完全访问
    // （图标 + 中文，还原 Shift+Tab 权限位的可视化标识）
    if (mode != "plan") {
        const PermSpec pspec = (permission == "bypass") ? kPermTable[1] : kPermTable[0];
        const std::string_view ic =
            (permission == "bypass") ? theme::icon_full_access() : theme::icon_manual();
        seg.push_back(ftxui::text("  "));
        seg.push_back(ftxui::color(pspec.color)(
            ftxui::text(std::string(ic) + " " + std::string(pspec.label))));
    }

    // #24：待办进度位（✓ X/Y，仅当有待办时显示）
    if (todo_total > 0) {
        seg.push_back(ftxui::text("  "));
        const std::string todo_text = std::format("✓ {}/{}", todo_done, todo_total);
        seg.push_back(ftxui::text(todo_text) | ftxui::color(theme::T::DiffAdd));
    }

    if (!model.empty()) {
        seg.push_back(ftxui::text("  "));
        seg.push_back(ftxui::text(model) | ftxui::color(theme::T::Text));
    }

    return ftxui::hbox(std::move(seg));
}

}  // namespace ftxtui
