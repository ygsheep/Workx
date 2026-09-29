/**
 * @file island_event_bridge.h
 * @brief EventBus → JSONL 事件桥（TUI 侧）
 * @details 订阅 agent 事件与 island 内部事件，转为协议事件后交给
 *          IslandServer 推送。订阅回调在主循环线程（M-8 drain）执行，
 *          本桥只做序列化 + 入队，无阻塞 I/O。
 *
 *          事件映射（与 GUI 的协议契约，见 plan/2026-08-11-island-ipc.md §5）：
 *          UserInputEvent → task_started
 *          StreamTokenEvent → thinking_delta / message_delta
 *          ToolCallEvent → tool_call
 *          ToolResultEvent → tool_result
 *          StreamDoneEvent → llm_done
 *          AgentDoneEvent → agent_done
 *          StreamErrorEvent → error
 *          CompactionPausedEvent → compaction_paused
 *          CacheDiagnosticsEvent → cache_diag
 *          island::CostUpdatedEvent → cost_updated
 *          island::BalanceUpdatedEvent → balance_updated
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/events/event_token.h"
#include "core/events/i_event_bus.h"
#include "island/island_server.h"

// 前瞻声明（头文件不展开 agent 事件定义，避免依赖面扩散）
namespace agent {
struct UserInputEvent;
struct StreamTokenEvent;
struct ToolCallEvent;
struct ToolResultEvent;
struct StreamDoneEvent;
struct AgentDoneEvent;
struct StreamErrorEvent;
struct CompactionPausedEvent;
struct CacheDiagnosticsEvent;
}  // namespace agent

namespace island {

/// @brief 事件桥
class IslandEventBridge {
   public:
    /// @param bus 事件总线（订阅源）
    /// @param server 事件输出端
    IslandEventBridge(agent::IEventBus& bus, IslandServer& server);

    IslandEventBridge(const IslandEventBridge&) = delete;
    IslandEventBridge& operator=(const IslandEventBridge&) = delete;

    /// @brief 析构自动退订（防御：异常路径或生产代码遗漏 unsubscribe_all）
    ~IslandEventBridge() { unsubscribe_all(); }

    /// @brief 取消全部订阅
    void unsubscribe_all();

    /// @brief 最近 tool_call 的 call_id → tool_name 映射条数（测试用）
    [[nodiscard]] size_t tool_name_cache_size() const { return m_tool_names.size(); }

   private:
    void on_user_input(const agent::UserInputEvent& e);
    void on_stream_token(const agent::StreamTokenEvent& e);
    void on_tool_call(const agent::ToolCallEvent& e);
    void on_tool_result(const agent::ToolResultEvent& e);
    void on_stream_done(const agent::StreamDoneEvent& e);
    void on_agent_done(const agent::AgentDoneEvent& e);
    void on_stream_error(const agent::StreamErrorEvent& e);
    void on_compaction_paused(const agent::CompactionPausedEvent& e);
    void on_cache_diag(const agent::CacheDiagnosticsEvent& e);
    void on_cost_updated(const struct CostUpdatedEvent& e);
    void on_balance_updated(const struct BalanceUpdatedEvent& e);

    agent::IEventBus& m_bus;
    IslandServer& m_server;
    /// @brief 订阅（type_index + token，unsubscribe_all 用）
    std::vector<std::pair<std::type_index, agent::EventToken>> m_tokens;
    /// @brief tool_call 的 call_id → tool_name（ToolResultEvent 仅携带 call_id）
    std::unordered_map<std::string, std::string> m_tool_names;
    static constexpr size_t kToolNameCacheMax = 256;
};

}  // namespace island