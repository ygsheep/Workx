/**
 * @file main.cpp
 * @brief 外部消费者冒烟测试（Issue #21 验收：无 TUI 驱动 Agent 循环）
 * @details 仅通过公共 API（workx::agent / workx::core）驱动 ChatSession：
 *          注入 fake provider → send_message → 订阅 StreamDoneEvent → 断言回复。
 *          不包含任何 tui/app 头文件，链接目标仅 workx_agent。
 */

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "agent/api/chat_types.h"
#include "agent/api/i_completion_provider.h"
#include "agent/api/i_stream_reader.h"
#include "agent/core/chat_session.h"
#include "core/config/config_manager.h"
#include "core/events/agent_events.h"
#include "core/events/event_bus.h"
#include "core/events/i_event_bus.h"
#include "core/events/stream_events.h"
#include "core/task/task_manager.h"

namespace {

/// @brief 最小 fake 流读取器：返回一段固定正文后 Complete
class FakeStreamReader final : public agent::IStreamReader {
   public:
    agent::StreamState next(std::function<bool()> should_stop, agent::StreamChunk& out) override {
        if (should_stop && should_stop()) return agent::StreamState::Cancelled;
        if (consumed_) {
            out = agent::StreamChunk{};
            out.is_final = true;
            out.prompt_tokens = 10;
            out.generated_tokens = 4;
            return agent::StreamState::Complete;
        }
        consumed_ = true;
        out = agent::StreamChunk{};
        out.content_delta = "Hello from consumer";
        return agent::StreamState::HasData;
    }
    void cancel() override {}

   private:
    bool consumed_ = false;
};

/// @brief 最小 fake provider：每次提交返回一段固定回复
class FakeProvider final : public agent::ICompletionProvider {
   public:
    std::shared_ptr<agent::IStreamReader> submit_completion(
        const agent::CompletionRequest& /*request*/) override {
        return std::make_shared<FakeStreamReader>();
    }
    void interrupt() override {}
    bool is_generating() const override { return false; }
};

}  // namespace

int main() {
    using namespace agent;

    // ---- 1. 装配最小宿主依赖（全部来自公共 API）----
    EventBus& event_bus = EventBus::instance();
    TaskManager& task_manager = TaskManager::instance();
    ConfigManager& config_manager = ConfigManager::instance();

    auto session = std::make_unique<ChatSession>(std::make_unique<FakeProvider>(), task_manager,
                                                 event_bus, config_manager, 1000, "consumer-smoke");

    // ---- 2. 订阅流完成事件，驱动主循环 drain ----
    std::atomic<bool> done{false};
    std::string reply;
    auto token = event_bus.subscribe<StreamDoneEvent>([&](const StreamDoneEvent& e) {
        reply = e.full_content;
        done.store(true);
    });

    session->send_message("ping");

    // ---- 3. 主循环：drain 异步事件直到完成或超时（模拟无 UI 宿主）----
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!done.load()) {
        event_bus.drain_async_events();
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    event_bus.unsubscribe<StreamDoneEvent>(token);

    if (!done.load()) {
        std::cerr << "FAIL: no StreamDoneEvent within 10s\n";
        return 1;
    }
    if (reply.find("Hello from consumer") == std::string::npos) {
        std::cerr << "FAIL: unexpected reply: " << reply << "\n";
        return 1;
    }

    std::cout << "OK: agent loop driven without TUI, reply='" << reply << "'\n";
    return 0;
}
