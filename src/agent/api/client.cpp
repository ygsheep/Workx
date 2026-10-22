/**
 * @file client.cpp
 * @brief Client 实现 — agent/api 顶层封装
 * @version 1.1.0
 * @date 2026-07
 */

#include "agent/api/client.h"

#include <chrono>
#include <cassert>
#include <stdexcept>
#include <thread>

#include "agent/api/i_backend.h"
#include "agent/api/backend_factory.h"
#include "agent/model/provider_preset.h"
#include "core/task/task_manager.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"

namespace agent {

// ============================================================
// create() — 静态工厂
// ============================================================

ResultV2<Client> Client::create(ClientConfig cfg) {
    const ProviderPreset* preset = nullptr;
    // 1. 若指定了 provider preset，查表填充 backend 默认值
    if (!cfg.provider.empty()) {
        preset = find_preset(cfg.provider);
        if (!preset) {
            return ResultV2<Client>::err(Error::Code::InvalidInput,
                                         "Unknown provider preset: " + cfg.provider, cfg.provider);
        }
        if (cfg.backend.base_url.empty()) {
            cfg.backend.base_url = std::string(preset->default_url);
        }
        cfg.backend.provider = preset->type;
        if (cfg.model.empty()) {
            cfg.model = std::string(preset->default_model);
        }
    }

    // 2. 模型名覆盖
    if (!cfg.model.empty()) {
        cfg.backend.model_name = cfg.model;
    }

    // 3. 校验必要字段
    if (cfg.backend.base_url.empty()) {
        return ResultV2<Client>::err(Error::Code::InvalidInput, "base_url is required");
    }
    cfg.backend.type = BackendConfig::Type::Remote;

    // 3.5 校验 api_key — Remote 后端必须有 api_key，但自定义 URL（openai-compatible）允许为空。
    if (cfg.backend.api_key.empty()) {
        bool needs_key = true;
        if (preset && preset->name == "openai-compatible") {
            needs_key = false;
        }
        if (needs_key) {
            return ResultV2<Client>::err(Error::Code::AuthenticationFailed,
                                         "API key is required for remote backend "
                                         "(set via --api-key or config)");
        }
    }

    // 4. H-4：校验 event_bus 与 enable_event_bus 一致性
    //    - enable_event_bus=true 必须提供 event_bus（否则构造期 *nullptr）
    //    - enable_event_bus=false 时 event_bus 应为 nullptr（避免误用）
    //    不再回退 EventBus::instance()，确保测试不污染全局单例（C-1 修复）
    if (cfg.enable_event_bus && cfg.event_bus == nullptr) {
        return ResultV2<Client>::err(Error::Code::InvalidInput,
                                     "enable_event_bus=true requires non-null event_bus");
    }

    // 5. 创建后端（H-1：透传 cfg.event_bus；nullptr 时 RemoteBackend 不发布
    //    BackendStatusEvent，符合 H-1 语义——不发布 ≠ 回退单例发布）
    auto backend = BackendFactory::create(cfg.backend, cfg.event_bus);
    if (!backend) {
        return ResultV2<Client>::err(Error::Code::InternalError, "Failed to create backend");
    }

    // 6. 初始化后端（V2-3：backend->initialize 返回 ResultV2）
    auto init_result = backend->initialize(cfg.backend);
    if (init_result.is_err()) {
        return ResultV2<Client>::err(init_result.error());
    }

    // 7. 构造 Client（M-1：DI 参数前置；H-4：显式注入 TaskManager::instance()）
    Client client(std::move(backend), TaskManager::instance(), cfg.event_bus, cfg.enable_event_bus,
                  std::move(cfg.system_prompt), cfg.retry_count, cfg.retry_delay_ms);
    return ResultV2<Client>::ok(std::move(client));
}

// ============================================================
// 构造 / 析构 / 移动
// ============================================================

// M-1：参数顺序调整为 DI 必需参数在前、可选参数在后
//      （backend, task_manager, event_bus, publish_events 为 DI；
//       system_prompt, retry_count, retry_delay_ms 为业务配置）
Client::Client(std::unique_ptr<IBackend> backend, ITaskManager& task_manager, IEventBus* event_bus,
               bool publish_events, std::string system_prompt, int retry_count, int retry_delay_ms)
    : m_backend(std::move(backend)),
      m_system_prompt(std::move(system_prompt))
      // H-3：委托给 HttpRetryPolicy 统一管理
      ,
      m_retry_policy{.max_retries = retry_count, .base_delay_ms = retry_delay_ms},
      m_publish_events(publish_events),
      m_task_manager(&task_manager),
      m_event_bus(event_bus) {
    // C-2：构造期强制校验契约——与 PR #7 ToolContext 策略一致
    if (m_publish_events && m_event_bus == nullptr) {
        throw std::logic_error("Client: publish_events=true requires non-null event_bus");
    }
    if (m_publish_events) {
        // 订阅 InterruptEvent → 自动中断
        m_interrupt_token =
            this->event_bus().subscribe<InterruptEvent>([this](const InterruptEvent&) {
                if (m_backend) {
                    m_backend->interrupt();
                }
            });
        m_subscribed = true;
    }
}

// H-4：依赖解析（不再回退单例；m_publish_events=true 时要求 m_event_bus 非空）
// issue #15-F: 从 assert 改为 throw，避免 Debug 构建直接 abort
IEventBus& Client::event_bus() const {
    if (!m_event_bus) {
        throw std::runtime_error("Client::event_bus() called with null m_event_bus");
    }
    return *m_event_bus;
}

Client::~Client() {
    // L-6：移动后 m_task_manager 可能为 nullptr，正常析构不应出现
    // 若 m_backend 存在却无 task_manager，说明对象状态异常
    // issue #15-F: 析构函数中不抛异常（否则触发 std::terminate），改为日志记录
    if (m_backend && m_task_manager == nullptr) {
        LOG_ERROR("Client::~Client: m_task_manager null but m_backend alive (moved-from?)");
    }

    if (m_subscribed && m_interrupt_token.is_valid()) {
        // H-4：m_subscribed=true 隐含 m_publish_events=true，m_event_bus 必须非空
        event_bus().unsubscribe<InterruptEvent>(m_interrupt_token);
    }
    if (m_backend) {
        m_backend->interrupt();
    }
    // 等待后台任务完成，防止 use-after-free
    // m_generating 会被 on_done/on_error 重置，等待其变 false（最长 30 秒兜底）
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (m_generating.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (m_backend) {
        m_backend->shutdown();
    }
}

Client::Client(Client&& other) noexcept
    : m_backend(std::move(other.m_backend)),
      m_messages(std::move(other.m_messages)),
      m_system_prompt(std::move(other.m_system_prompt)),
      m_generating(other.m_generating.load()),
      m_retry_policy(other.m_retry_policy),
      m_publish_events(other.m_publish_events),
      m_task_manager(other.m_task_manager),
      m_event_bus(other.m_event_bus),
      m_interrupt_token(std::move(other.m_interrupt_token)),
      m_subscribed(other.m_subscribed) {
    // C-3：移动后源对象必须处于"不发布"状态，否则在源对象上调用
    // submit/on_*/析构 等路径会触发 *m_event_bus（已被置 nullptr）
    other.m_subscribed = false;
    other.m_generating.store(false);
    other.m_task_manager = nullptr;
    other.m_event_bus = nullptr;
    other.m_publish_events = false;
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        // 清理当前订阅
        if (m_subscribed && m_interrupt_token.is_valid()) {
            event_bus().unsubscribe<InterruptEvent>(m_interrupt_token);
        }
        if (m_backend) {
            m_backend->interrupt();
            m_backend->shutdown();
        }

        m_backend = std::move(other.m_backend);
        {
            std::lock_guard<std::mutex> lock(m_messages_mutex);
            m_messages = std::move(other.m_messages);
            m_system_prompt = std::move(other.m_system_prompt);
        }
        m_generating.store(other.m_generating.load());
        m_retry_policy = other.m_retry_policy;
        m_publish_events = other.m_publish_events;
        m_task_manager = other.m_task_manager;
        m_event_bus = other.m_event_bus;
        m_interrupt_token = std::move(other.m_interrupt_token);
        m_subscribed = other.m_subscribed;

        // C-3：同步重置源对象状态
        other.m_subscribed = false;
        other.m_generating.store(false);
        other.m_task_manager = nullptr;
        other.m_event_bus = nullptr;
        other.m_publish_events = false;
    }
    return *this;
}

// ============================================================
// build_request — 构造 CompletionRequest
// ============================================================

CompletionRequest Client::build_request(const std::string& user_text) {
    CompletionRequest req;
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        if (!m_system_prompt.empty()) {
            req.messages.push_back(ChatMessage::system(m_system_prompt));
        }
        req.messages.insert(req.messages.end(), m_messages.begin(), m_messages.end());
    }
    req.messages.push_back(ChatMessage::user(user_text));
    return req;
}

CompletionRequest Client::build_request(const std::vector<ChatMessage>& messages) {
    CompletionRequest req;
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        if (!m_system_prompt.empty()) {
            req.messages.push_back(ChatMessage::system(m_system_prompt));
        }
    }
    req.messages.insert(req.messages.end(), messages.begin(), messages.end());
    return req;
}

// ============================================================
// run_stream — 核心流式逻辑（阻塞版共享）
// ============================================================

ResultV2<void> Client::run_stream(const CompletionRequest& request, const ChatCallbacks& cbs,
                                  const std::function<bool()>& should_stop,
                                  std::string& content_out, std::string& reasoning_out) {
    // B.3：拆分后的 run_stream 仅做顶层重试调度
    // H-3：重试次数和退避延迟统一由 m_retry_policy 管理
    for (int attempt = 0; attempt <= m_retry_policy.max_retries; ++attempt) {
        // 每次重试前清空已累积输出，避免新旧内容拼接错乱
        content_out.clear();
        reasoning_out.clear();

        if (should_stop()) {
            return ResultV2<void>::ok();
        }

        auto reader = m_backend->submit_completion(request);
        if (!reader) {
            // B.3：submit 失败 → 委托给 handle_submit_failure
            int64_t delay = m_retry_policy.delay_ms(attempt);
            if (handle_submit_failure(attempt, delay, cbs, should_stop)) {
                continue;  // 已触发重试
            }
            // 被中断 or 重试耗尽
            if (should_stop()) {
                return ResultV2<void>::ok();  // 被中断 → ok
            }
            return ResultV2<void>::err(Error::Code::InternalError,
                                       "Failed to submit completion request after retries");
        }

        // 拉取流式响应
        StreamChunk chunk;
        while (true) {
            if (should_stop()) {
                reader->cancel();
                break;
            }

            auto state = reader->next(should_stop, chunk);

            // K-2：非 TUI 场景下若开启 EventBus 集成，需在同步阻塞期间
            // 消费异步事件并清理已完成任务，避免事件滞留 / TaskManager 内存累积
            if (m_publish_events) {
                event_bus().process_async_events();
                m_task_manager->update();
            }

            if (state == StreamState::HasData) {
                if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                    content_out += chunk.content_delta;
                    reasoning_out += chunk.reasoning_delta;

                    if (cbs.on_token) {
                        cbs.on_token(chunk.content_delta, chunk.reasoning_delta);
                    }
                    publish_token_event(chunk);
                }
            } else if (state == StreamState::Complete) {
                // 处理可能的最后一块数据
                if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                    content_out += chunk.content_delta;
                    reasoning_out += chunk.reasoning_delta;
                    if (cbs.on_token) {
                        cbs.on_token(chunk.content_delta, chunk.reasoning_delta);
                    }
                }
                if (cbs.on_done) {
                    cbs.on_done(chunk);
                }
                publish_done_event(content_out, reasoning_out, false, chunk);
                return ResultV2<void>::ok();
            } else if (state == StreamState::Cancelled) {
                if (cbs.on_done) {
                    StreamChunk final_chunk = chunk;
                    final_chunk.is_final = true;
                    cbs.on_done(final_chunk);
                }
                publish_done_event(content_out, reasoning_out, true, chunk);
                return ResultV2<void>::ok();
            } else if (state == StreamState::Error) {
                // B.3：流错误 → 委托给 handle_stream_error
                if (handle_stream_error(attempt, cbs, should_stop)) {
                    break;  // 已触发重试，break 内层 while 进入下次重试
                }
                // 被中断 or 重试耗尽
                if (should_stop()) {
                    return ResultV2<void>::ok();  // 被中断 → ok
                }
                return ResultV2<void>::err(Error::Code::StreamError, "Stream error after retries");
            }
        }
    }

    return ResultV2<void>::ok();
}

// ============================================================
// B.3：拆分出的子方法实现
// ============================================================

// L-3：compute_backoff_delay_ms 已删除（冗余封装）。
//      原实现仅委托 m_retry_policy.delay_ms(attempt)，且从未被调用
//      （重试退避由 handle_submit_failure/handle_stream_error 内部直接使用
//       HttpRetryPolicy）。如需退避延迟，直接调用 HttpRetryPolicy::delay_ms()。

bool Client::handle_submit_failure(int attempt, int64_t delay_ms, const ChatCallbacks& cbs,
                                   const std::function<bool()>& should_stop) {
    // 返回值约定：true = 已触发重试（调用方 continue）；false = 应退出（重试耗尽或被中断）
    // 被中断时调用方通过 should_stop() 判断后返回 ok（与原逻辑一致）
    if (attempt >= m_retry_policy.max_retries) {
        // 重试耗尽
        if (cbs.on_error) {
            cbs.on_error("Failed to submit completion request after retries", false);
        }
        if (m_publish_events) {
            event_bus().publish_async(
                StreamErrorEvent{.session_id = "client",
                                 .message = "Failed to submit completion request after retries",
                                 .retryable = false});
        }
        return false;
    }

    if (cbs.on_error) {
        cbs.on_error("Submit failed, retrying in " + std::to_string(delay_ms) + "ms... (" +
                         std::to_string(attempt + 1) + "/" +
                         std::to_string(m_retry_policy.max_retries) + ")",
                     true);
    }
    if (m_publish_events) {
        event_bus().publish_async(StreamErrorEvent{
            .session_id = "client", .message = "Submit failed, retrying...", .retryable = true});
    }
    // 可中断的退避等待：被中断时返回 false，调用方检查 should_stop 后返回 ok
    if (interruptible_sleep(std::chrono::milliseconds(delay_ms), should_stop)) {
        return false;  // 被中断
    }
    return true;  // 正常结束，调用方 continue 重试
}

bool Client::handle_stream_error(int attempt, const ChatCallbacks& cbs,
                                 const std::function<bool()>& should_stop) {
    // 返回值约定：true = 已触发重试（调用方 break 内层 while）；false = 应退出
    if (attempt >= m_retry_policy.max_retries) {
        if (cbs.on_error) {
            cbs.on_error("Stream error after retries", false);
        }
        if (m_publish_events) {
            event_bus().publish_async(StreamErrorEvent{.session_id = "client",
                                                       .message = "Stream error after retries",
                                                       .retryable = false});
        }
        return false;
    }

    int64_t delay = m_retry_policy.delay_ms(attempt);
    if (cbs.on_error) {
        cbs.on_error("Stream error, retrying...", true);
    }
    if (interruptible_sleep(std::chrono::milliseconds(delay), should_stop)) {
        return false;  // 被中断
    }
    return true;  // 调用方 break 进入下次重试
}

void Client::publish_token_event(const StreamChunk& chunk) const {
    if (!m_publish_events) return;
    event_bus().publish_async(StreamTokenEvent{.session_id = "client",
                                               .content_delta = chunk.content_delta,
                                               .reasoning_delta = chunk.reasoning_delta,
                                               .is_thinking = !chunk.reasoning_delta.empty(),
                                               .token_count = chunk.token_count});
}

void Client::publish_done_event(const std::string& content, const std::string& reasoning,
                                bool was_interrupted, const StreamChunk& chunk) const {
    if (!m_publish_events) return;
    event_bus().publish_async(StreamDoneEvent{.session_id = "client",
                                              .full_content = content,
                                              .full_reasoning = reasoning,
                                              .was_interrupted = was_interrupted,
                                              .prompt_tokens = chunk.prompt_tokens,
                                              .generated_tokens = chunk.generated_tokens,
                                              .prompt_ms = chunk.prompt_ms,
                                              .generation_ms = chunk.generation_ms});
}

// ============================================================
// interruptible_sleep
// ============================================================

bool Client::interruptible_sleep(std::chrono::milliseconds duration,
                                 const std::function<bool()>& should_stop) {
    auto wait_until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < wait_until) {
        if (should_stop()) {
            return true;  // 被中断
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;  // 正常结束
}

// ============================================================
// 阻塞 API
// ============================================================

ResultV2<std::string> Client::chat(const std::string& user_text) {
    auto request = build_request(user_text);
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        m_messages.push_back(ChatMessage::user(user_text));
    }

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, {}, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.is_ok()) {
        {
            std::lock_guard<std::mutex> lock(m_messages_mutex);
            m_messages.push_back(ChatMessage::assistant(content));
        }
        return ResultV2<std::string>::ok(std::move(content));
    }
    // 失败时回滚 user 消息，避免孤儿消息污染历史
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::User) {
            m_messages.pop_back();
        }
    }
    return ResultV2<std::string>::err(result.error());
}

ResultV2<std::string> Client::chat(const std::vector<ChatMessage>& messages) {
    auto request = build_request(messages);

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, {}, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.is_ok()) {
        return ResultV2<std::string>::ok(std::move(content));
    }
    return ResultV2<std::string>::err(result.error());
}

ResultV2<void> Client::stream_chat(const std::string& user_text, const ChatCallbacks& cbs) {
    auto request = build_request(user_text);
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        m_messages.push_back(ChatMessage::user(user_text));
    }

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, cbs, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.is_ok()) {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        m_messages.push_back(ChatMessage::assistant(content));
    } else {
        // 失败回滚
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::User) {
            m_messages.pop_back();
        }
    }
    return result;
}

ResultV2<void> Client::stream_chat(const std::vector<ChatMessage>& messages,
                                   const ChatCallbacks& cbs) {
    auto request = build_request(messages);

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, cbs, []() { return false; }, content, reasoning);
    m_generating.store(false);

    return result;
}

// ============================================================
// 异步 API
// ============================================================

ResultV2<void> Client::chat_async(const std::string& user_text, const ChatCallbacks& cbs) {
    // chat_async 不再包装，直接委托 stream_chat_async
    // stream_chat_async 已负责累积 content 并 push assistant 消息
    return stream_chat_async(user_text, cbs);
}

ResultV2<void> Client::stream_chat_async(const std::string& user_text, const ChatCallbacks& cbs) {
    // 用 compare_exchange_strong 修复 TOCTOU
    bool expected = false;
    if (!m_generating.compare_exchange_strong(expected, true)) {
        return ResultV2<void>::err(Error::Code::InternalError, "Already generating");
    }

    auto request = build_request(user_text);
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        m_messages.push_back(ChatMessage::user(user_text));
    }

    // 捕获 content 用于记录 assistant 消息
    auto content_ptr = std::make_shared<std::string>();
    auto reasoning_ptr = std::make_shared<std::string>();

    ChatCallbacks wrapped_cbs;
    wrapped_cbs.on_token = [cbs, content_ptr, reasoning_ptr](const std::string& content_delta,
                                                             const std::string& reasoning_delta) {
        *content_ptr += content_delta;
        *reasoning_ptr += reasoning_delta;
        if (cbs.on_token) {
            cbs.on_token(content_delta, reasoning_delta);
        }
    };
    auto original_on_done = cbs.on_done;
    wrapped_cbs.on_done = [original_on_done, content_ptr, this](const StreamChunk& final_chunk) {
        {
            std::lock_guard<std::mutex> lock(m_messages_mutex);
            m_messages.push_back(ChatMessage::assistant(*content_ptr));
        }
        m_generating.store(false);
        if (original_on_done) {
            original_on_done(final_chunk);
        }
    };
    auto original_on_error = cbs.on_error;
    wrapped_cbs.on_error = [original_on_error, this](const std::string& msg, bool retryable) {
        m_generating.store(false);
        if (original_on_error) {
            original_on_error(msg, retryable);
        }
    };

    // 启动后台任务
    m_task_manager->launch(
        "client_completion", [this, request = std::move(request), cbs = std::move(wrapped_cbs),
                              content_ptr, reasoning_ptr](const std::atomic<bool>& should_cancel) {
            std::string content, reasoning;
            run_stream(
                request, cbs, [&should_cancel]() { return should_cancel.load(); }, content,
                reasoning);
            // 确保 generating 被重置（on_done/on_error 可能未触发）
            m_generating.store(false);
        });

    return ResultV2<void>::ok();
}

// ============================================================
// 会话管理
// ============================================================

void Client::set_system_prompt(const std::string& prompt) {
    std::lock_guard<std::mutex> lock(m_messages_mutex);
    m_system_prompt = prompt;
}

void Client::clear_history() {
    std::lock_guard<std::mutex> lock(m_messages_mutex);
    m_messages.clear();
}

void Client::regenerate() {
    // 生成中拒绝 regenerate，避免与后台 push_back 竞态
    if (m_generating.load()) {
        if (m_publish_events) {
            event_bus().publish_async(
                StreamErrorEvent{.session_id = "client",
                                 .message = "Still generating, cannot regenerate",
                                 .retryable = true});
        }
        return;
    }
    std::lock_guard<std::mutex> lock(m_messages_mutex);
    // 移除最后一条 assistant 消息
    if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::Assistant) {
        m_messages.pop_back();
    }
}

std::vector<ChatMessage> Client::history() const {
    std::lock_guard<std::mutex> lock(m_messages_mutex);
    return m_messages;
}

// ============================================================
// 控制
// ============================================================

void Client::interrupt() {
    if (m_backend) {
        m_backend->interrupt();
    }
}

bool Client::is_generating() const { return m_generating.load(); }

// ============================================================
// 后端能力透传
// ============================================================

ResultV2<std::vector<ModelInfo>> Client::list_models() { return m_backend->list_models(); }

void Client::set_model(const std::string& name) { m_backend->set_model_name(name); }

std::string Client::model_name() const { return m_backend->get_model_info().name; }

}  // namespace agent
