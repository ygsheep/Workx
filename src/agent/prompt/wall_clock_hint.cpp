/**
 * @file wall_clock_hint.cpp
 * @brief 剩余墙钟收敛提示（Issue #166 方案 C）实现
 */

#include "agent/prompt/wall_clock_hint.h"

#include <format>

#include "agent/prompt/budget_hint.h"  // 复用 format_duration

namespace agent::prompt {

WallClockTier wall_clock_tier(double remaining_sec, int reserve_sec) {
    // 未启用墙钟（未配置预算 / 阈值非正）→ 整体关闭，绝不改变交互式场景行为
    if (reserve_sec <= 0) {
        return WallClockTier::Normal;
    }
    const double reserve = static_cast<double>(reserve_sec);
    // 已越过 deadline（remaining <= 0）同样落在 Critical：让模型立刻交卷，
    // 哪怕只剩一次 Thought 的机会，也好过被外部超时直接杀掉。
    if (remaining_sec <= reserve / 3.0) {
        return WallClockTier::Critical;
    }
    if (remaining_sec <= reserve) {
        return WallClockTier::Warn;
    }
    return WallClockTier::Normal;
}

std::string format_wall_clock_hint(double remaining_sec, WallClockTier tier,
                                   std::string_view verify_command) {
    if (tier == WallClockTier::Normal) {
        return {};
    }

    const std::string left = format_duration(remaining_sec);
    std::string hint = std::format("[墙钟提醒] 本次运行的总墙钟预算只剩约 {}。", left);

    if (tier == WallClockTier::Critical) {
        // 与 #83 的 Critical 同款取舍：这一刻要的是「交卷」，不是「再跑一遍测试」，
        // 因此刻意不再提验证命令，避免两条指令互相打架。
        hint +=
            "即将被外部超时终止，且超时会被判定为失败：立即停止一切工具调用与新的修改，"
            "把当前已完成的成果、产出文件与尚未完成的清单直接整理成最终答复输出。";
        return hint;
    }

    hint += "请立刻转入收敛与收尾：不要开启新的探索性改动或扩大范围，";
    if (verify_command.empty()) {
        hint += "运行一次项目的构建/测试命令确认当前成果，然后给出最终答复。";
    } else {
        hint += std::format("先运行 `{}` 确认当前成果，再给出最终答复。", verify_command);
    }
    hint += "超时会被外部硬性终止，届时已完成的成果不会被验证。";
    return hint;
}

}  // namespace agent::prompt
