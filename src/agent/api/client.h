/**
 * @file client.h
 * @brief Client — agent/api 顶层封装入口
 * @details 收拢 BackendFactory + RemoteBackend + 会话管理为一站式 API。
 *          支持 ClientConfig 指定初始化器构造、阻塞/异步两套聊天 API、
 *          可选 EventBus 集成（兼容现有 TUI ChatRenderer）。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

#include "core/utils/result.h"
#include "core/utils/result_v2.h"  // V2-3：新 ResultV2
#include "core/events/event_bus.h"
#include "agent/api/chat_types.h"
#include "agent/api/retry.h"  // H-3：HttpRetryPolicy
#include "agent/model/provider_type.h"
#include "core/task/task_manager.h"  // ITaskManager + TaskManager::instance() 默认实参

namespace agent {

class IBackend;
class IEventBus;

// ============================================================
// ChatCallbacks
// ============================================================

/// @brief 流式回调集合
struct ChatCallbacks {
    /// @brief 收到增量 token（回复或思考）
    /// @param content_delta 正文增量
    /// @param reasoning_delta 推理/思考增量
    std::function<void(const std::string& content_delta, const std::string& reasoning_delta)>
        on_token;

    /// @brief 流式完成（正常结束），final_chunk.is_final == true
    std::function<void(const StreamChunk& final_chunk)> on_done;

    /// @brief 流途中错误（提交成功后的流错误）
    /// @param msg 错误信息
    /// @param retryable 是否可重试
    std::function<void(const std::string& msg, bool retryable)> on_error;
};

// ============================================================
// ClientConfig
// ============================================================

/// @brief Client 配置（聚合体，支持指定初始化器）
/// @details 用法：Client::create({.provider = "lm-studio", .model = "..."})
struct ClientConfig {
    /// Provider 预设名（如 "lm-studio"/"deepseek"/"openai"）。
    /// 非空时自动查 ProviderPreset 填充 backend 的 base_url/provider/默认 model。
    /// 留空时所有 backend 字段需手动设置。
    std::string provider;

    /// 模型名（显式设置优先于 preset 默认值，覆盖 backend.model_name）
    std::string model;

    /// 内层后端配置（base_url / api_key / timeout_ms / provider 等）
    BackendConfig backend;

    /// 系统提示词（可选）
    std::string system_prompt;

    /// 重试次数与初始退避延迟（毫秒，指数退避）
    int retry_count = 3;
    int retry_delay_ms = 1000;

    /// 开启后：订阅 InterruptEvent 自动中断 + 发布 StreamToken/Done/Error
    /// C-1：enable_event_bus=true 时 event_bus 必须非空，否则 Client::create 返回 err
    bool enable_event_bus = false;

    /// H-4：事件总线注入（C-1：不再回退单例）
    /// @details - enable_event_bus=true 时必须非空，否则 Client::create 返回 InvalidInput
    ///          - enable_event_bus=false 时可为 nullptr，RemoteBackend 也不发布
    ///            BackendStatusEvent（"不发布" ≠ "回退单例发布"）
    IEventBus* event_bus = nullptr;
};

// ============================================================
// Client
// ============================================================

/// @brief 顶层聊天客户端 — 封装 BackendFactory + 会话管理
/// @details 调用方不直接接触 IBackend / ChatSession / EventBus。
class Client {
   public:
    /// @brief 从配置创建 Client
    /// @details 内部完成：查 preset（若 provider 非空）→ 拼 BackendConfig
    ///          → BackendFactory::create → backend->initialize
    /// @return 失败返回 Error（不抛异常）
    /// @details V2-3：从 Result<Client, string> 迁移到 ResultV2<Client>
    /// @code
    /// auto r = Client::create({.provider = "lm-studio",
    ///                          .model = "google/gemma-4-e4b"});
    /// if (r.is_err()) { std::cerr << r.error().message; return; }
    /// Client client = std::move(r.value());
    /// @endcode
    static ResultV2<Client> create(ClientConfig config);

    // ---- 会话管理 ----
    void set_system_prompt(const std::string& prompt);
    void clear_history();
    void regenerate();
    [[nodiscard]] std::vector<ChatMessage> history() const;

    // ---- 阻塞 API（脚本/CLI 场景）----
    // 在调用线程同步执行，回调在该线程触发。调用期间线程被阻塞。
    // 适合一次性脚本；TUI 主线程勿用（会冻结界面）。
    /// @details V2-3：返回 ResultV2，错误通过 Error 携带错误码
    ResultV2<std::string> chat(const std::string& user_text);
    ResultV2<std::string> chat(const std::vector<ChatMessage>& messages);
    ResultV2<void> stream_chat(const std::string& user_text, const ChatCallbacks& cbs);
    ResultV2<void> stream_chat(const std::vector<ChatMessage>& messages, const ChatCallbacks& cbs);

    // ---- 异步 API（TUI/交互场景）----
    // 立即返回，推理在 TaskManager 后台任务中执行。
    // 回调在后台线程触发 → 调用方需处理线程安全。
    // TUI 场景建议开启 enable_event_bus，用 EventBus 回主线程渲染。
    // 返回 Ok 表示已提交；提交级失败返回 Err。
    /// @details V2-3：返回 ResultV2，错误通过 Error 携带错误码
    ResultV2<void> chat_async(const std::string& user_text, const ChatCallbacks& cbs);
    ResultV2<void> stream_chat_async(const std::string& user_text, const ChatCallbacks& cbs);

    // ---- 控制 ----
    void interrupt();
    [[nodiscard]] bool is_generating() const;

    // ---- 后端能力透传 ----
    /// @details V2-3：返回 ResultV2，错误通过 Error 携带错误码
    ResultV2<std::vector<ModelInfo>> list_models();
    void set_model(const std::string& name);
    [[nodiscard]] std::string model_name() const;

    // 仅可移动（手动实现，因 std::atomic 不可移动）
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

   private:
    // M-1：参数顺序调整为 DI 必需参数在前、可选参数在后
    Client(std::unique_ptr<IBackend> backend, ITaskManager& task_manager, IEventBus* event_bus,
           bool publish_events, std::string system_prompt, int retry_count, int retry_delay_ms);

    /// 构造 CompletionRequest（含 system_prompt + history + 新消息）
    CompletionRequest build_request(const std::string& user_text);
    CompletionRequest build_request(const std::vector<ChatMessage>& messages);

    /// 流式核心实现（阻塞版共享）
    /// @param should_stop 外部取消检查
    /// @details V2-3：返回 ResultV2，错误通过 Error 携带错误码
    ResultV2<void> run_stream(const CompletionRequest& request, const ChatCallbacks& cbs,
                              const std::function<bool()>& should_stop, std::string& content_out,
                              std::string& reasoning_out);

    /// @brief 可中断的睡眠（用于重试退避等待）
    /// @return true 表示睡眠期间 should_stop() 变为 true（应退出）
    bool interruptible_sleep(std::chrono::milliseconds duration,
                             const std::function<bool()>& should_stop);

    /// @brief B.3：处理 submit 失败的错误回调与事件发布
    /// @return true 表示已触发重试（调用方应 continue 重试循环）；false 表示重试耗尽（调用方应返回
    /// err）
    bool handle_submit_failure(int attempt, int64_t delay_ms, const ChatCallbacks& cbs,
                               const std::function<bool()>& should_stop);

    /// @brief B.3：处理流式错误的回调与事件发布
    /// @return true 表示已触发重试（调用方应 break 内层循环进入下次重试）；false 表示重试耗尽
    bool handle_stream_error(int attempt, const ChatCallbacks& cbs,
                             const std::function<bool()>& should_stop);

    /// @brief B.3：发布 StreamTokenEvent（仅 m_publish_events=true 时）
    void publish_token_event(const StreamChunk& chunk) const;

    /// @brief B.3：发布流式完成事件（仅 m_publish_events=true 时）
    void publish_done_event(const std::string& content, const std::string& reasoning,
                            bool was_interrupted, const StreamChunk& chunk) const;

    std::unique_ptr<IBackend> m_backend;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::atomic<bool> m_generating{false};
    // H-3：用 HttpRetryPolicy 统一重试配置，替代散落的 m_max_retries/m_retry_delay_ms
    HttpRetryPolicy m_retry_policy;
    bool m_publish_events = false;

    // D-1：任务管理器指针（非拥有；Client 可移动，用指针避免引用无法重新绑定）
    ITaskManager* m_task_manager = nullptr;

    // H-4：事件总线指针（非拥有；m_publish_events=true 时必须非空，
    //     由 Client::create 显式注入，nullptr 时不应触发事件发布路径）
    IEventBus* m_event_bus = nullptr;

    /// @brief 解析事件总线（H-4：不再回退单例，要求 m_publish_events=true 时 m_event_bus 非空）
    /// C-2：Debug 编译期通过 assert 检查不变量；构造期通过 throw std::logic_error 强制
    IEventBus& event_bus() const;

    /// 保护 m_messages 和 m_system_prompt 的互斥量
    mutable std::mutex m_messages_mutex;

    /// EventBus 订阅 token（仅 m_publish_events=true 时有效）
    EventToken m_interrupt_token;
    bool m_subscribed = false;
};

}  // namespace agent
