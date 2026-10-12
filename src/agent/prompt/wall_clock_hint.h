/**
 * @file wall_clock_hint.h
 * @brief 剩余墙钟收敛提示（Issue #166 方案 C / #137 建议 3）
 * @details 与 #83 的 `budget_hint` 互补：#83 管的是**轮次**预算（还剩几轮），
 *          本模块管的是**墙钟**预算（还剩几秒）。#137 实测「轮次还很充裕、
 *          墙钟却被耗尽」才是评测场景的真实瓶颈 —— agent 不知道自己有 deadline，
 *          一路做到被外层（harbor）硬超时杀掉，trial 记 `AgentTimeoutError`，
 *          已完成的成果白白丢掉。本模块在剩余墙钟进入阈值后注入一条 system 提示，
 *          把模型从「继续探索」推向「收尾 + 验证」。
 *
 *          档位（以剩余秒数与收敛阈值 `reserve_sec` 判定）：
 *          - Warn     剩余 <= reserve_sec：停止扩展范围，跑一次验证后收尾
 *          - Critical 剩余 <= reserve_sec / 3：立即整理成果交卷，不再调用工具
 *          - Normal   剩余 >  reserve_sec：不注入（避免每轮塞噪音进上下文）
 *
 * @note `reserve_sec <= 0` 时**整个模块关闭**（返回 Normal）：墙钟预算未配置
 *       （交互式场景）时不应因本模块改变任何行为。
 * @note 全部为纯函数：不读配置、不碰时钟、不起子进程，便于直接单测。
 * @version 0.1.0
 * @date 2026-10
 */

#pragma once

#include <string>
#include <string_view>

namespace agent::prompt {

/// @brief 墙钟档位
enum class WallClockTier {
    Normal,    ///< 墙钟充裕（> reserve_sec）：不注入提示
    Warn,      ///< 转入收尾（<= reserve_sec）
    Critical,  ///< 立即交卷（<= reserve_sec / 3）
};

/// @brief 判定墙钟档位
/// @param remaining_sec 剩余墙钟（秒，可为负 —— 已越过 deadline 同样按 Critical）
/// @param reserve_sec   收敛阈值（秒）。<= 0 表示未启用墙钟，一律 Normal
/// @return 档位；Normal 表示无需注入
WallClockTier wall_clock_tier(double remaining_sec, int reserve_sec);

/// @brief 生成要注入对话的墙钟收敛提示
/// @param remaining_sec   剩余墙钟（秒，负数按 0 处理）
/// @param tier            档位（Normal 返回空串）
/// @param verify_command  Warn 档建议跑的验证命令（空 = 只做泛化要求）。
///                        Critical 档刻意忽略它 —— 那一刻要的是交卷而不是再跑测试。
/// @return 提示文本；Normal 档（及退化输入）返回空串，调用方据此跳过注入
std::string format_wall_clock_hint(double remaining_sec, WallClockTier tier,
                                   std::string_view verify_command);

}  // namespace agent::prompt
