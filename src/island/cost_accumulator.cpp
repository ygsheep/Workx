/**
 * @file cost_accumulator.cpp
 * @brief 费用累积器实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "island/cost_accumulator.h"

#include "core/events/agent_events.h"
#include "core/events/stream_events.h"

namespace island {

namespace {

/// @brief 按单价表精确折算；模型未匹配时回退 deepseek-chat 并标记估算
/// @return nullptr 表示单价表无任何可用条目（跳过该事件，仅日志）
const ModelPricing* resolve_pricing(const PricingTable& table, const std::string& model,
                                    bool& is_estimated) {
    if (const auto* p = table.find(model)) {
        is_estimated = p->model != model;
        return p;
    }
    if (const auto* fallback = table.find("deepseek-chat")) {
        is_estimated = true;
        return fallback;
    }
    return nullptr;
}

}  // namespace

CostAccumulator::CostAccumulator(agent::IEventBus& bus, PricingTable pricing, std::string model)
    : m_bus(bus), m_pricing(std::move(pricing)), m_model(std::move(model)) {
    m_tokens.push_back(bus.subscribe<agent::UserInputEvent>(
        [this](const agent::UserInputEvent& e) { on_user_input(e); }));
    m_tokens.push_back(bus.subscribe<agent::StreamDoneEvent>(
        [this](const agent::StreamDoneEvent& e) { on_stream_done(e); }));
    m_tokens.push_back(bus.subscribe<agent::AgentDoneEvent>(
        [this](const agent::AgentDoneEvent& e) { on_agent_done(e); }));
}

CostAccumulator::~CostAccumulator() {
    m_bus.unsubscribe<agent::UserInputEvent>(m_tokens[0]);
    m_bus.unsubscribe<agent::StreamDoneEvent>(m_tokens[1]);
    m_bus.unsubscribe<agent::AgentDoneEvent>(m_tokens[2]);
}

CostSnapshot CostAccumulator::snapshot() const {
    CostSnapshot s;
    s.task_cost = m_task_cost;
    s.session_cost = m_session_cost;
    s.is_estimated = m_is_estimated;
    s.model = m_model;
    return s;
}

void CostAccumulator::set_on_task_completed(std::function<void()> cb) {
    m_on_task_completed = std::move(cb);
}

CostBreakdown CostAccumulator::calc_delta(const ModelPricing& pricing, int input_tokens,
                                          int output_tokens, int cache_read_tokens,
                                          int cache_write_tokens) {
    constexpr double kPerMillion = 1'000'000.0;
    CostBreakdown delta;
    delta.input_usd = input_tokens / kPerMillion * pricing.input_per_1m;
    delta.output_usd = output_tokens / kPerMillion * pricing.output_per_1m;
    delta.cache_read_usd = cache_read_tokens / kPerMillion * pricing.cache_read_per_1m;
    delta.cache_write_usd = cache_write_tokens / kPerMillion * pricing.cache_write_per_1m;
    delta.total_usd =
        delta.input_usd + delta.output_usd + delta.cache_read_usd + delta.cache_write_usd;
    return delta;
}

void CostAccumulator::on_user_input(const agent::UserInputEvent& e) {
    if (e.is_local_command) return;
    // 新任务开始：清空上一任务的残留（上一任务异常中断未触发 agent_done 时保证干净）
    m_task_cost = {};
}

void CostAccumulator::on_stream_done(const agent::StreamDoneEvent& e) {
    if (e.is_local_command) return;
    const int input = e.prompt_cache_miss_tokens;
    const int cache_read = e.prompt_cache_hit_tokens;
    const int output = e.generated_tokens;
    const int cache_write = e.cache_creation_input_tokens;
    if (input == 0 && output == 0 && cache_read == 0 && cache_write == 0) return;

    bool estimated = false;
    const ModelPricing* pricing = resolve_pricing(m_pricing, m_model, estimated);
    if (!pricing) return;
    m_is_estimated = estimated;

    const CostBreakdown delta = calc_delta(*pricing, input, output, cache_read, cache_write);
    m_task_cost += delta;
    m_session_cost += delta;
    m_has_cost = true;
    publish_update();
}

void CostAccumulator::on_agent_done(const agent::AgentDoneEvent&) {
    // session 已在 on_stream_done 同步累积，此处仅结算本任务
    m_task_cost = {};
    if (m_has_cost) {
        publish_update();
    }
    if (m_on_task_completed) {
        m_on_task_completed();
    }
}

void CostAccumulator::publish_update() {
    CostSnapshot s;
    s.task_cost = m_task_cost;
    s.session_cost = m_session_cost;
    s.is_estimated = m_is_estimated;
    s.model = m_model;
    CostUpdatedEvent ev{s};
    m_bus.publish_async(ev);
}

}  // namespace island