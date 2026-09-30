/**
 * @file backend_types.h
 * @brief 后端配置与模型信息类型（P1 从 chat_types.h 拆分）
 * @details BackendConfig（后端配置）和 ModelInfo（模型信息）属于"后端管理"领域，
 *          与 ChatMessage/CompletionRequest/StreamChunk 等"聊天消息"领域解耦。
 *          chat_types.h 已 #include 本文件作为向后兼容 shim。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <cstdint>
#include "agent/model/provider_type.h"

namespace agent {

// ============================================================
// BackendConfig
// ============================================================

/// @brief 后端配置
struct BackendConfig {
    enum class Type { Remote, Local };

    Type type = Type::Remote;
    ProviderType provider = ProviderType::OpenAI;  ///< API 协议类型

    // Remote 配置
    std::string base_url;    ///< API 基础 URL
    std::string api_key;     ///< API Key
    std::string model_name;  ///< 模型名称
    int timeout_ms = 30000;  ///< HTTP 超时（毫秒）
    bool send_reasoning_content =
        false;  ///< DS_CACHE P2：是否往返 reasoning_content（DeepSeek-reasoner）

    // Local 配置（Phase 5）
    std::string model_path;
    int n_ctx = 4096;
    int n_gpu_layers = -1;
};

// ============================================================
// ModelInfo
// ============================================================

/// @brief 模型信息
struct ModelInfo {
    std::string name;
    std::string description;
    int32_t context_length = 0;
};

// ============================================================
// BackendState (M-7)
// ============================================================

/// @brief 后端运行态（M-7：合并 m_ready/m_generating 两个 bool 为单一 enum）
/// @details 原 RemoteBackend 用两个 std::atomic<bool> 分别表示"已初始化"与"生成中"，
///          存在状态组合歧义（如 m_ready=false 但 m_generating=true 属于非法态）。
///          M-7 合并为单一枚举，原子读写保证状态一致性，消除非法组合。
enum class BackendState {
    /// @brief 初始未初始化态（L-A：仅用于构造初始值；shutdown 后转 Shutdown 而非 Idle）
    /// @details 状态转换图：Idle →(initialize)→ Ready →(submit)→ Generating →(完成)→ Ready
    /// →(shutdown)→ Shutdown
    Idle,
    Ready,       ///< 已初始化，可接受请求
    Generating,  ///< 正在生成推理结果
    Shutdown  ///< 已显式 shutdown，不可恢复（与 Idle 区别：Idle 可 initialize，Shutdown 不可）
};

}  // namespace agent
