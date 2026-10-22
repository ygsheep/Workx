/**
 * @file remote_backend.cpp
 * @brief 远程后端实现
 * @details 使用 HttpClient（Boost.Beast）发送 API 请求，通过 IProviderAdapter 适配不同协议
 * @version 4.1.0
 * @date 2026-07
 */

#include "agent/api/remote/remote_backend.h"
#include "agent/api/provider/openai_adapter.h"
#include "agent/api/provider/anthropic_adapter.h"
// H-1：不再 include core/events/event_bus.h，避免重新引入 EventBus::instance() 依赖
#include "agent/message/types.h"

#include <format>
#include <nlohmann/json.hpp>

#include <liblogger/logger.h>

#include <algorithm>
#include <stdexcept>
#include <iostream>

namespace agent {

// ============================================================
// RemoteBackend 实现
// ============================================================

RemoteBackend::~RemoteBackend() { shutdown(); }

ResultV2<void> RemoteBackend::initialize(const BackendConfig& config) {
    if (config.type != BackendConfig::Type::Remote) {
        return ResultV2<void>::err(Error::Code::InvalidInput,
                                   "RemoteBackend requires Remote config type");
    }
    if (config.base_url.empty()) {
        return ResultV2<void>::err(Error::Code::InvalidInput,
                                   "base_url is required for RemoteBackend");
    }

    m_config = config;

    // 根据 ProviderType 创建对应的协议适配器
    switch (config.provider) {
        case ProviderType::OpenAI: {
            auto openai_adapter = std::make_unique<OpenAIAdapter>();
            // DS_CACHE P2：把 reasoning_content 往返配置传给 adapter
            openai_adapter->set_send_reasoning_content(config.send_reasoning_content);
            m_adapter = std::move(openai_adapter);
            break;
        }
        case ProviderType::Anthropic:
            m_adapter = std::make_unique<AnthropicAdapter>();
            break;
        default:
            return ResultV2<void>::err(Error::Code::InternalError, "Unknown provider type");
    }

#ifdef WORKX_HAS_CURL
    // M-1：测试注入的客户端优先保留，否则创建真实 HttpClient
    if (!m_http_client) {
        m_http_client = std::make_unique<HttpClient>();
    }
    m_state.store(BackendState::Ready, std::memory_order_release);

    // H-1：通过 DI 注入的 m_event_bus 发布，不再调用 EventBus::instance()；
    // 为 nullptr 时跳过发布（保持向后兼容）
    if (m_event_bus) {
        m_event_bus->publish_async(BackendStatusEvent{
            .status = BackendStatusEvent::Connected, .backend_name = name(), .error = {}});
    }

    return ResultV2<void>::ok();
#else
    return ResultV2<void>::err(
        Error::Code::NotImplemented,
        "RemoteBackend requires CURL. Check CURL installation and reconfigure");
#endif
}

void RemoteBackend::shutdown() {
    // M-A：状态仲裁全程持有 m_active_mutex，消除 interrupt() 与 store(Shutdown) 之间的 TOCTOU
    // 竞态。
    //      原实现：interrupt() 释放锁后、store(Shutdown) 前另一线程可 CAS Ready→Generating
    //      创建新 reader，随后 store(Shutdown) 覆盖但新 reader 未清理。
    //      现实现：持锁后 CAS 状态，interrupt_locked() 在同一锁内摘除 reader，无窗口。
    //
    // #139：锁的作用域**只覆盖状态仲裁与集合摘除**，取消动作必须移到锁外
    //      （cancel_readers_out_of_lock）。原实现在锁内调 interrupt_locked() →
    //      cancel_stream() → on_complete 回调 → 回调再次 lock(m_active_mutex)，
    //      同线程二次加锁非递归 mutex = 永久阻塞（表现为任务做完但进程不退出）。
    std::vector<std::shared_ptr<SSEStreamReader>> victims;
    bool cascaded = false;  // 是否已完成状态转换（Ready/Generating → Shutdown）
    {
        std::lock_guard<std::mutex> lock(m_active_mutex);

        // 尝试 Ready → Shutdown（常见路径：空闲时关闭）
        BackendState expected = BackendState::Ready;
        if (m_state.compare_exchange_strong(expected, BackendState::Shutdown,
                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
            // Ready 态无 active_reader
            cascaded = true;
        } else {
            // 尝试 Generating → Shutdown（生成中关闭：先摘除 reader 再 shutdown）
            expected = BackendState::Generating;
            if (m_state.compare_exchange_strong(expected, BackendState::Shutdown,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
                // CAS 成功：状态已转 Shutdown，此时不会有新 submit_completion 进入（非 Ready）
                victims = interrupt_locked();  // 锁内只摘集合，取消在锁外
                cascaded = true;
            }
        }
        // 其他状态（Idle / Shutdown）：不做任何操作
        // - Idle：未初始化，无资源需清理
        // - Shutdown：已 shutdown，幂等返回
    }

    // #139：锁已释放 —— cancel_stream 在此触发 on_complete 回调是安全的
    cancel_readers_out_of_lock(victims);

    if (!cascaded) return;
    if (m_http_client) {
        m_http_client->shutdown();
    }
    if (m_event_bus) {
        m_event_bus->publish_async(BackendStatusEvent{
            .status = BackendStatusEvent::Disconnected, .backend_name = name(), .error = {}});
    }
}

ModelInfo RemoteBackend::get_model_info() const {
    return ModelInfo{.name = m_config.model_name, .description = "Remote API", .context_length = 0};
}

std::shared_ptr<IStreamReader> RemoteBackend::submit_completion(const CompletionRequest& request) {
#ifdef WORKX_HAS_CURL
    // v1.2.0（PR #49 修复）：支持并发在飞请求（子 Agent 并行批量调度）。
    // 原实现单活跃请求（m_active_reader 非空即拒绝），并行子任务中仅第一个
    // 能获得 LLM 响应，其余立即失败。HttpClient 基于 curl_multi 支持并发流，
    // 此处快速路径只检查基础设施，状态仲裁在锁内完成。
    if (!m_adapter || !m_http_client ||
        m_state.load(std::memory_order_acquire) == BackendState::Shutdown) {
        return nullptr;
    }

    // 创建 SSE 流读取器，传入 Provider 特定的解析回调
    auto parse_cb = [this](const std::string& event_type, const std::string& data,
                           StreamChunk& out) -> bool {
        return m_adapter->parse_sse_event(event_type, data, out);
    };

    auto reader = std::make_shared<SSEStreamReader>(std::move(parse_cb));

    // 锁内仲裁并发提交：
    //  - 首个请求：CAS Ready → Generating（失败即状态已变，如并发 shutdown，拒绝）
    //  - 后续请求：要求仍处于 Generating（非终态），直接加入集合
    {
        std::lock_guard<std::mutex> lock(m_active_mutex);
        if (m_active_readers.empty()) {
            BackendState expected = BackendState::Ready;
            if (!m_state.compare_exchange_strong(expected, BackendState::Generating,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
                return nullptr;
            }
        } else if (m_state.load(std::memory_order_acquire) != BackendState::Generating) {
            return nullptr;
        }
        m_active_readers.push_back(reader);
    }

    std::string url = m_adapter->build_url(m_config.base_url);
    std::string body = m_adapter->build_request_body(request, m_config.model_name);
    auto header_pairs = m_adapter->build_headers(m_config.api_key);

    // 通过 HttpClient 发送异步流式 POST
    // on_complete 在 HttpClient poll 线程触发，需加锁与 submit_completion/interrupt 同步
    m_http_client->async_post_stream(
        url, header_pairs, body, reader,
        [this, reader]() {
            std::lock_guard<std::mutex> lock(m_active_mutex);
            // v1.2.0（PR #49 修复）：从集合移除本请求，而非无条件 reset 单一 reader
            std::erase(m_active_readers, reader);
            // M-N1：仅当集合清空且仍为 Generating 时才回到 Ready，避免覆盖 Shutdown 终态。
            // 边界场景：shutdown() 持锁 CAS Generating→Shutdown 并清理 reader 后释放锁，
            // 被取消的请求触发 on_complete 时若用 store(Ready) 会覆盖 Shutdown，导致
            // backend 回到 Ready 但 http_client 已 shutdown，后续请求接受但失败。
            // CAS 失败（状态非 Generating，如已被 shutdown 转为 Shutdown）则保持终态。
            if (m_active_readers.empty()) {
                BackendState expected = BackendState::Generating;
                m_state.compare_exchange_strong(expected, BackendState::Ready,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire);
            }
        },
        m_config.timeout_ms);

    // 直接返回 shared_ptr<IStreamReader>，避免 SharedPtrWrapper 适配层
    return reader;

#else
    (void)request;
    return nullptr;
#endif
}

void RemoteBackend::interrupt() {
    std::vector<std::shared_ptr<SSEStreamReader>> victims;
    {
        std::lock_guard<std::mutex> lock(m_active_mutex);
        victims = interrupt_locked();
    }
    // #139：取消必须在锁外 —— cancel_stream 会同步触发 on_complete 回调，
    //       而该回调要 lock(m_active_mutex)，持锁调用即自锁。
    cancel_readers_out_of_lock(victims);
}

std::vector<std::shared_ptr<SSEStreamReader>> RemoteBackend::interrupt_locked() {
    // M-A：interrupt 的无锁实现，调用方必须已持有 m_active_mutex
    // v1.2.0（PR #49 修复）：中断全部在飞请求（支持并发批量调度）
    //
    // #139：只把集合整体摘下并复位状态，**不在这里取消**。取消会同步走到
    //       on_complete 回调，而回调要再次 lock(m_active_mutex) —— 调用方正持锁，
    //       同线程二次加锁非递归 mutex = 永久阻塞。
    auto victims = std::move(m_active_readers);
    m_active_readers.clear();
    // M-7：若处于 Generating，回到 Ready；其他状态不变
    BackendState expected = BackendState::Generating;
    m_state.compare_exchange_strong(expected, BackendState::Ready, std::memory_order_acq_rel,
                                    std::memory_order_acquire);
    return victims;
}

void RemoteBackend::cancel_readers_out_of_lock(
    const std::vector<std::shared_ptr<SSEStreamReader>>& readers) {
    // #139：调用方必须已释放 m_active_mutex
    for (const auto& reader : readers) {
        if (!reader) continue;
        reader->cancel();
        if (m_http_client) {
            m_http_client->cancel_stream(reader.get());
        }
    }
}

// ============================================================
// list_models 实现
// ============================================================

ResultV2<std::vector<ModelInfo>> RemoteBackend::list_models() {
#ifdef WORKX_HAS_CURL
    // M-7：Ready 态才允许查询模型列表
    if (m_state.load(std::memory_order_acquire) != BackendState::Ready || !m_adapter ||
        !m_http_client) {
        return ResultV2<std::vector<ModelInfo>>::err(Error::Code::InternalError,
                                                     "Backend not ready");
    }

    // 检查 provider 是否支持 list_models HTTP 端点
    // Anthropic 无公开 list models 端点，返回内置模型列表
    auto endpoint = m_adapter->get_models_endpoint();
    if (!endpoint.supported) {
        auto builtin = m_adapter->get_builtin_models(m_config.base_url);
        if (!builtin.empty()) {
            return ResultV2<std::vector<ModelInfo>>::ok(std::move(builtin));
        }
        return ResultV2<std::vector<ModelInfo>>::err(
            Error::Code::NotImplemented, "This provider does not support list_models endpoint");
    }

    // 构建 URL（用 endpoint.url_suffix，如 "/v1/models"）
    std::string url = m_config.base_url;
    if (url.empty()) {
        return ResultV2<std::vector<ModelInfo>>::err(Error::Code::ConfigInvalid,
                                                     "base_url is empty");
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += endpoint.url_suffix;

    auto header_pairs = m_adapter->build_headers(m_config.api_key);

    // debug: 记录请求信息
    LOG_INFO("[debug/models] GET {}", url);
    for (const auto& [k, v] : header_pairs) {
        // 安全：对所有可能的认证头脱敏，避免泄露 api key
        std::string k_lower = k;
        std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
        std::string val;
        if (k_lower == "authorization" || k_lower == "x-api-key" || k_lower == "x-goog-api-key" ||
            k_lower.find("key") != std::string::npos ||
            k_lower.find("token") != std::string::npos) {
            val = "***";
        } else {
            val = v;
        }
        LOG_INFO("[debug/models]  {}:{} ", k, val);
    }

    // 同步 GET 请求（V2-2：get 返回 ResultV2<HttpResponse>）
    auto result = m_http_client->get(url, header_pairs, 15000);

    // V2-2：网络错误（curl 失败、无法到达服务器）通过 Error 携带错误码
    if (result.is_err()) {
        const auto& err = result.error();
        LOG_ERROR("[backend] list_models network error: {}", err.to_string());
        // 直接传播 Error，保留错误码（NetworkTimeout/NetworkDisconnected 等）
        return ResultV2<std::vector<ModelInfo>>::err(err);
    }

    const auto& response = result.value();

    // HTTP 4xx/5xx 错误（已到达服务器，但服务端返回错误状态码）
    if (response.is_http_error()) {
        return ResultV2<std::vector<ModelInfo>>::err(
            Error::from_http_response(response.status_code, response.body, url));
    }

    // 解析 JSON 响应
    try {
        auto json = nlohmann::json::parse(response.body);
        std::vector<ModelInfo> models;

        if (json.contains("data") && json["data"].is_array()) {
            for (const auto& item : json["data"]) {
                ModelInfo info;
                info.name = item.value("id", "unknown");
                info.description = item.value("owned_by", "");
                if (item.contains("context_length") && item["context_length"].is_number()) {
                    info.context_length = item["context_length"].get<int32_t>();
                }
                models.push_back(std::move(info));
            }
        }

        if (models.empty()) {
            return ResultV2<std::vector<ModelInfo>>::err(Error::Code::InternalError,
                                                         "No models returned by API");
        }

        return ResultV2<std::vector<ModelInfo>>::ok(std::move(models));
    } catch (const nlohmann::json::parse_error& e) {
        return ResultV2<std::vector<ModelInfo>>::err(
            Error::Code::ConfigParseFailed, std::format("JSON parse error: {}", e.what()), url);
    }

#else
    return ResultV2<std::vector<ModelInfo>>::err(
        Error::Code::NotImplemented,
        "RemoteBackend requires CURL. Check CURL installation and reconfigure");
#endif
}

}  // namespace agent
