/**
 * @file island_event_bridge.cpp
 * @brief 事件桥实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "island/island_event_bridge.h"

#include "core/events/agent_events.h"
#include "core/events/stream_events.h"
#include "core/tool_kind.h"
#include "island/events.h"

namespace island {

namespace {

std::string tool_type_name(agent::tool::ToolType type) {
    switch (type) {
        case agent::tool::ToolType::ReadFile:
            return "read_file";
        case agent::tool::ToolType::WriteFile:
            return "write_file";
        case agent::tool::ToolType::EditFile:
            return "edit_file";
        case agent::tool::ToolType::Execute:
            return "execute";
        case agent::tool::ToolType::Search:
            return "search";
        case agent::tool::ToolType::Agent:
            return "agent";
        default:
            return "other";
    }
}

std::string truncate_utf8(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return std::string(s);
    size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) {
        --end;
    }
    std::string out(s.substr(0, end));
    out += "...";
    return out;
}

nlohmann::json cost_breakdown_json(const CostBreakdown& c) {
    return nlohmann::json{
        {"input_usd", c.input_usd},           {"output_usd", c.output_usd},
        {"cache_read_usd", c.cache_read_usd}, {"cache_write_usd", c.cache_write_usd},
        {"total_usd", c.total_usd},
    };
}

}  // namespace

IslandEventBridge::IslandEventBridge(agent::IEventBus& bus, IslandServer& server)
    : m_bus(bus), m_server(server) {
    const auto sub = [this](std::type_index type, agent::EventToken token) {
        m_tokens.emplace_back(type, std::move(token));
    };
    sub(typeid(agent::UserInputEvent),
        bus.subscribe<agent::UserInputEvent>(
            [this](const agent::UserInputEvent& e) { on_user_input(e); }));
    sub(typeid(agent::StreamTokenEvent),
        bus.subscribe<agent::StreamTokenEvent>(
            [this](const agent::StreamTokenEvent& e) { on_stream_token(e); }));
    sub(typeid(agent::ToolCallEvent),
        bus.subscribe<agent::ToolCallEvent>(
            [this](const agent::ToolCallEvent& e) { on_tool_call(e); }));
    sub(typeid(agent::ToolResultEvent),
        bus.subscribe<agent::ToolResultEvent>(
            [this](const agent::ToolResultEvent& e) { on_tool_result(e); }));
    sub(typeid(agent::StreamDoneEvent),
        bus.subscribe<agent::StreamDoneEvent>(
            [this](const agent::StreamDoneEvent& e) { on_stream_done(e); }));
    sub(typeid(agent::AgentDoneEvent),
        bus.subscribe<agent::AgentDoneEvent>(
            [this](const agent::AgentDoneEvent& e) { on_agent_done(e); }));
    sub(typeid(agent::StreamErrorEvent),
        bus.subscribe<agent::StreamErrorEvent>(
            [this](const agent::StreamErrorEvent& e) { on_stream_error(e); }));
    sub(typeid(agent::CompactionPausedEvent),
        bus.subscribe<agent::CompactionPausedEvent>(
            [this](const agent::CompactionPausedEvent& e) { on_compaction_paused(e); }));
    sub(typeid(agent::CacheDiagnosticsEvent),
        bus.subscribe<agent::CacheDiagnosticsEvent>(
            [this](const agent::CacheDiagnosticsEvent& e) { on_cache_diag(e); }));
    sub(typeid(CostUpdatedEvent),
        bus.subscribe<CostUpdatedEvent>([this](const CostUpdatedEvent& e) { on_cost_updated(e); }));
    sub(typeid(BalanceUpdatedEvent),
        bus.subscribe<BalanceUpdatedEvent>(
            [this](const BalanceUpdatedEvent& e) { on_balance_updated(e); }));
}

void IslandEventBridge::unsubscribe_all() {
    for (auto& [type, token] : m_tokens) {
        m_bus.unsubscribe_raw(type, token);
    }
    m_tokens.clear();
}

void IslandEventBridge::on_user_input(const agent::UserInputEvent& e) {
    if (e.is_local_command) return;
    // 避免把整段 @file 展开文本推给 GUI，仅预览开头
    m_server.publish_event("task_started",
                           nlohmann::json{{"text_preview", truncate_utf8(e.text, 200)}});
}

void IslandEventBridge::on_stream_token(const agent::StreamTokenEvent& e) {
    if (!e.reasoning_delta.empty()) {
        m_server.publish_event("thinking_delta", nlohmann::json{{"delta_text", e.reasoning_delta}});
    }
    if (!e.content_delta.empty() && !e.is_thinking) {
        m_server.publish_event("message_delta", nlohmann::json{{"delta_text", e.content_delta}});
    }
}

void IslandEventBridge::on_tool_call(const agent::ToolCallEvent& e) {
    if (m_tool_names.size() >= kToolNameCacheMax &&
        m_tool_names.find(e.call_id) == m_tool_names.end()) {
        m_tool_names.clear();  // 防膨胀：超限清空重来
    }
    m_tool_names[e.call_id] = e.tool_name;
    m_server.publish_event("tool_call", nlohmann::json{
                                            {"call_id", e.call_id},
                                            {"tool_name", e.tool_name},
                                            {"tool_type", tool_type_name(e.tool_type)},
                                            {"arguments", truncate_utf8(e.arguments, 200)},
                                        });
}

void IslandEventBridge::on_tool_result(const agent::ToolResultEvent& e) {
    const auto it = m_tool_names.find(e.call_id);
    const std::string tool_name = it != m_tool_names.end() ? it->second : "";
    if (it != m_tool_names.end()) {
        m_tool_names.erase(it);
    }
    m_server.publish_event("tool_result", nlohmann::json{
                                              {"call_id", e.call_id},
                                              {"tool_name", tool_name},
                                              {"is_error", e.is_error},
                                              {"result_preview", truncate_utf8(e.result, 200)},
                                          });
}

void IslandEventBridge::on_stream_done(const agent::StreamDoneEvent& e) {
    // 每次 LLM 调用发布一次：内嵌 token 计数供 GUI 展示与费用对账
    const nlohmann::json tokens{
        {"input", e.prompt_cache_miss_tokens},
        {"output", e.generated_tokens},
        {"cache_read", e.prompt_cache_hit_tokens},
        {"cache_write", e.cache_creation_input_tokens},
        {"prompt_tokens", e.prompt_tokens},
        {"cache_read_input_tokens", e.cache_read_input_tokens},
    };
    m_server.publish_event("llm_done", nlohmann::json{
                                           {"tokens", tokens},
                                           {"was_interrupted", e.was_interrupted},
                                           {"generation_ms", e.generation_ms},
                                       });
}

void IslandEventBridge::on_agent_done(const agent::AgentDoneEvent& e) {
    m_server.publish_event("agent_done",
                           nlohmann::json{
                               {"total_steps", e.total_steps},
                               {"total_tool_calls", e.total_tool_calls},
                               {"total_duration_ms", e.total_duration_ms},
                               {"final_response_preview", truncate_utf8(e.final_response, 200)},
                           });
}

void IslandEventBridge::on_stream_error(const agent::StreamErrorEvent& e) {
    m_server.publish_event("error", nlohmann::json{
                                        {"message", e.message},
                                        {"retryable", e.retryable},
                                    });
}

void IslandEventBridge::on_compaction_paused(const agent::CompactionPausedEvent& e) {
    m_server.publish_event("compaction_paused",
                           nlohmann::json{
                               {"paused", e.paused},
                               {"ratio", e.ratio},
                               {"consecutive_compacts", e.consecutive_compacts},
                               {"notice", e.notice},
                           });
}

void IslandEventBridge::on_cache_diag(const agent::CacheDiagnosticsEvent& e) {
    m_server.publish_event("cache_diag", nlohmann::json{
                                             {"prefix_changed", e.prefix_changed},
                                             {"cache_hit_tokens", e.cache_hit_tokens},
                                             {"cache_miss_tokens", e.cache_miss_tokens},
                                             {"reasons", e.reasons},
                                         });
}

void IslandEventBridge::on_cost_updated(const CostUpdatedEvent& e) {
    m_server.publish_event("cost_updated",
                           nlohmann::json{
                               {"task_cost", cost_breakdown_json(e.snapshot.task_cost)},
                               {"session_cost", cost_breakdown_json(e.snapshot.session_cost)},
                               {"is_estimated", e.snapshot.is_estimated},
                               {"model", e.snapshot.model},
                           });
}

void IslandEventBridge::on_balance_updated(const BalanceUpdatedEvent& e) {
    m_server.publish_event("balance_updated", nlohmann::json{
                                                  {"success", e.result.success},
                                                  {"balance_usd", e.result.balance_usd},
                                                  {"cny_balance", e.result.cny_balance},
                                                  {"fetched_at", e.result.fetched_at},
                                                  {"error", e.result.error},
                                                  {"source", e.result.source},
                                              });
}

}  // namespace island