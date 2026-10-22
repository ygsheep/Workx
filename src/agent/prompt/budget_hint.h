/**
 * @file budget_hint.h
 * @brief 剩余预算提示（Issue #83）
 * @details 解决「Agent 不知道自己还剩多少预算」：按剩余轮次占总预算的比例分三档，
 *          进入低预算档位后向对话注入一条 system 提示，把模型从「继续改」
 *          推向「验证与收尾」。
 *
 *          档位（以剩余/总预算的百分比判定，整除，不留浮点误差）：
 *          - Warn     <= 30%：停止扩展范围，跑一次验证再收尾
 *          - Critical <= 10%：立即输出当前进展与未完成项，不再开启新修改
 *          - Normal   >  30%：不注入（避免每轮都塞一行噪音进上下文）
 *
 * @note 只有**轮次**维度是真实账本（源于 ReActLoop::Config::max_iterations）。
 *       金额维度（issue 原文写的 `剩余预算 $Y`）依赖费用累计，属 #55 的硬限侧，
 *       本模块尚未接入，故暂以**已用时**替代，让模型至少知道自己跑了多久。
 *
 * @note 全部为纯函数：不读配置、不碰文件系统、不起子进程，便于直接单测。
 */

#pragma once

#include <string>
#include <string_view>

namespace agent::prompt {

/// @brief 预算档位
enum class BudgetTier {
    Normal,    ///< 预算充裕（> 30%）：不注入提示
    Warn,      ///< 转入收尾（<= 30%）
    Critical,  ///< 立即交卷（<= 10%）
};

/// @brief 判定预算档位
/// @param remaining 剩余轮次（ReAct 循环扣减后的真实余量）
/// @param total     本轮总预算（ReActLoop::Config::max_iterations）
/// @return Normal 表示无需注入；total/remaining 非正等退化输入同样返回 Normal
BudgetTier budget_tier(int remaining, int total);

/// @brief 人类可读的时长串
/// @param seconds 秒（负数按 0 处理）
/// @return "45s" / "3m20s" / "1h2m5s"
std::string format_duration(double seconds);

/// @brief 生成要注入对话的预算提示
/// @param remaining       剩余轮次
/// @param total           本轮总预算
/// @param elapsed_seconds 循环已用时（秒）
/// @param verify_command  Warn 档建议跑的验证命令（空 = 只做泛化要求）
/// @return 提示文本；Normal 档（及退化输入）返回空串，调用方据此跳过注入
std::string format_budget_hint(int remaining, int total, double elapsed_seconds,
                               std::string_view verify_command);

}  // namespace agent::prompt
