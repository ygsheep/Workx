/**
 * @file events.h
 * @brief Island 内部事件类型（island 模块私有，不经 workx 公共 API）
 * @details CostAccumulator / BalanceFetcher 通过 EventBus 发布本文件的事件，
 *          IslandEventBridge 订阅后转换为 JSONL 推送给 GUI。
 *          GUI 不订阅时这些事件仅被 bridge 消费，无副作用。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>

namespace island {

/// @brief 费用分解（按计费维度拆分）
struct CostBreakdown {
    double input_usd = 0.0;        ///< 普通输入（cache miss）费用
    double output_usd = 0.0;       ///< 输出费用
    double cache_read_usd = 0.0;   ///< 缓存命中读费用
    double cache_write_usd = 0.0;  ///< 缓存写入费用
    double total_usd = 0.0;        ///< 合计

    CostBreakdown& operator+=(const CostBreakdown& o) {
        input_usd += o.input_usd;
        output_usd += o.output_usd;
        cache_read_usd += o.cache_read_usd;
        cache_write_usd += o.cache_write_usd;
        total_usd += o.total_usd;
        return *this;
    }
};

/// @brief 费用快照（当前任务 + 会话累计）
struct CostSnapshot {
    CostBreakdown task_cost;     ///< 当前任务（一个 user turn）
    CostBreakdown session_cost;  ///< 会话累计
    bool is_estimated = false;   ///< 模型未匹配单价表、按 fallback 估算
    std::string model;           ///< 当前模型名
};

/// @brief 余额拉取结果
struct BalanceResult {
    bool success = false;      ///< 拉取与解析是否成功
    double balance_usd = 0.0;  ///< 折算后的 USD 余额
    double cny_balance = 0.0;  ///< DeepSeek 返回的 CNY 余额
    int64_t fetched_at = 0;    ///< 拉取时间（Unix 秒）
    std::string error;         ///< 失败原因（成功时为空）
    std::string source;        ///< 数据来源（"deepseek" / "cache"）
};

/// @brief 费用更新事件（CostAccumulator 发布 → bridge 转发为 cost_updated）
struct CostUpdatedEvent {
    CostSnapshot snapshot;
};

/// @brief 余额更新事件（BalanceFetcher 发布 → bridge 转发为 balance_updated）
struct BalanceUpdatedEvent {
    BalanceResult result;
};

}  // namespace island