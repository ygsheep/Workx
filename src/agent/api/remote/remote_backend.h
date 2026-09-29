/**
 * @file remote_backend.h
 * @brief 远程后端（OpenAI/Anthropic HTTP/SSE）
 * @details 通过 IProviderAdapter 适配不同 API 协议，使用 HttpClient 发送请求
 * @version 4.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <vector>
#include "agent/api/i_backend.h"
#include "agent/api/remote/sse_stream_reader.h"
#include "agent/api/remote/http_client.h"
#include "agent/api/provider/i_provider_adapter.h"
#include "core/events/i_event_bus.h"

namespace agent {

/// @brief 远程后端
/// @details 连接 OpenAI 兼容或 Anthropic API，支持流式推理。
///          H-1：构造接收 IEventBus* 解除对 EventBus::instance() 的硬依赖；
///          为 nullptr 时不发布 BackendStatusEvent（保持向后兼容）。
class RemoteBackend : public IBackend {
   public:
    /// @brief 构造
    /// @param event_bus 事件总线（H-1 DI：nullptr 时不发布后端状态事件，
    ///                   由调用方显式注入；M-2：移除默认实参强制显式传参）
    explicit RemoteBackend(IEventBus* event_bus) : m_event_bus(event_bus) {}
    ~RemoteBackend() override;

    // IBackend 接口
    std::string name() const override { return "remote"; }
    ResultV2<void> initialize(const BackendConfig& config) override;
    void shutdown() override;
    bool is_ready() const override { return m_state.load() == BackendState::Ready; }
    ModelInfo get_model_info() const override;
    ResultV2<std::vector<ModelInfo>> list_models() override;
    void set_model_name(const std::string& name) override { m_config.model_name = name; }

    // ICompletionProvider 接口
    std::shared_ptr<IStreamReader> submit_completion(const CompletionRequest& request) override;
    void interrupt() override;
    bool is_generating() const override { return m_state.load() == BackendState::Generating; }

    /// @brief 获取当前后端状态（M-7：诊断 / 测试用）
    /// @details 替代原有的两个 bool 查询，单一原子读取保证状态一致快照
    [[nodiscard]] BackendState state() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }

    /// @brief 测试注入：替换内部 HTTP 客户端（M-1，须在 initialize 前调用）
    /// @details 默认 initialize 内部创建真实 HttpClient；测试注入 Fake 实现
    ///          以直接驱动 on_complete / cancel_stream，覆盖多 reader 并发仲裁路径。
    void set_http_client_for_testing(std::unique_ptr<IHttpClient> client) {
        m_http_client = std::move(client);
    }

   private:
    BackendConfig m_config;
    // M-7：合并 m_ready / m_generating 两个 atomic<bool> 为单一 atomic<BackendState>，
    //      消除"m_ready=false 但 m_generating=true"等非法组合，原子读写保证状态一致
    std::atomic<BackendState> m_state{BackendState::Idle};

    /// @brief 中断全部在飞请求（调用方必须已持有 m_active_mutex）
    /// @details 拆出 interrupt_locked 以便 shutdown() 在持锁状态下复用清理逻辑，
    ///          避免 interrupt() 内部再次加锁导致死锁，并消除 shutdown 与 interrupt 间的 TOCTOU
    ///          竞态。
    void interrupt_locked();

    /// @brief Provider 特定协议适配器
    std::unique_ptr<IProviderAdapter> m_adapter;

    /// @brief HTTP 客户端（M-1：经 IHttpClient 接口依赖，测试可注入 Fake）
    std::unique_ptr<IHttpClient> m_http_client;

    /// @brief 在飞请求的 reader 集合（v1.2.0 修复：支持并发在飞请求）
    /// @details 原单一 m_active_reader 仅允许一个在飞请求，与子 Agent 并行批量调度冲突
    ///          （第二个请求被拒绝）。HttpClient 基于 curl_multi 天然支持并发流，
    ///          此处改为集合管理，interrupt/shutdown 遍历取消全部。
    ///          m_active_mutex 保护本集合的读写，避免与 interrupt 并发竞态。
    mutable std::mutex m_active_mutex;
    std::vector<std::shared_ptr<SSEStreamReader>> m_active_readers;

    /// @brief H-1：DI 注入的事件总线（nullptr 时不发布后端状态事件）
    IEventBus* m_event_bus = nullptr;
};

}  // namespace agent
