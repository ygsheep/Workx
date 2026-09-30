/**
 * @file cost_accumulator.h
 * @brief 费用累积器（StreamDoneEvent token → USD）
 * @details 订阅 agent 事件，把每次 LLM 调用的 usage 按 PricingTable 折算 USD：
 *          - input       = prompt_cache_miss_tokens × input_per_1m  （cache miss 即写缓存价）
 *          - cache_read  = prompt_cache_hit_tokens × cache_read_per_1m
 *          - cache_write = cache_creation_input_tokens × cache_write_per_1m（Anthropic
 * 风格，DeepSeek 为 0）
 *          - output      = generated_tokens × output_per_1m
 *          任务边界：UserInputEvent（非本地命令）开启新任务 → task 清零；
 *          AgentDoneEvent 收尾 → task 并入 session、task 清零。
 *          费用发布走 publish_async（M-8：主循环 drain 派发，bridge 在主循环线程消费）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <string>

#include "core/events/event_token.h"
#include "core/events/i_event_bus.h"
#include "island/events.h"
#include "island/pricing_table.h"

// 前瞻声明（头文件不展开 agent 事件定义，避免依赖面扩散）
namespace agent {
struct UserInputEvent;
struct StreamDoneEvent;
struct AgentDoneEvent;
}  // namespace agent

namespace island {

/// @brief 费用累积器
class CostAccumulator {
   public:
    /// @param bus 事件总线（订阅 UserInput/StreamDone/AgentDone）
    /// @param pricing 单价表
    /// @param model 当前模型名（main 解析后传入；模型切换后无感知，重建场景可更新）
    CostAccumulator(agent::IEventBus& bus, PricingTable pricing, std::string model);

    CostAccumulator(const CostAccumulator&) = delete;
    CostAccumulator& operator=(const CostAccumulator&) = delete;

    /// @brief 析构时退订全部事件，避免 EventBus 悬挂回调（单例跨测试/模块共享）
    ~CostAccumulator();

    /// @brief 当前费用快照
    [[nodiscard]] CostSnapshot snapshot() const;

    /// @brief 任务完成回调（AgentDoneEvent 时触发，main 接线 BalanceFetcher.trigger_refresh）
    void set_on_task_completed(std::function<void()> cb);

   private:
    /// @brief 按单价表折算一次 usage（纯函数，供单测）
    static CostBreakdown calc_delta(const ModelPricing& pricing, int input_tokens,
                                    int output_tokens, int cache_read_tokens,
                                    int cache_write_tokens);

    void on_user_input(const agent::UserInputEvent& e);
    void on_stream_done(const agent::StreamDoneEvent& e);
    void on_agent_done(const agent::AgentDoneEvent& e);
    void publish_update();

    agent::IEventBus& m_bus;
    PricingTable m_pricing;
    std::string m_model;

    CostBreakdown m_task_cost;     ///< 当前任务（一个 user turn）
    CostBreakdown m_session_cost;  ///< 会话累计
    bool m_is_estimated = false;   ///< 当前模型是否有精确单价
    bool m_has_cost = false;       ///< 本会话是否产生过费用（避免发全 0 事件）

    std::vector<agent::EventToken> m_tokens;
    std::function<void()> m_on_task_completed;
};

}  // namespace island