/**
 * @file i_completion_provider.h
 * @brief 统一推理提供者接口
 * @details IBackend 和 IAgentCore 都实现此接口，ChatSession 只依赖此接口
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <memory>
#include "agent/api/chat_types.h"
#include "agent/api/i_stream_reader.h"
#include "core/export.h"

namespace agent {

/// @brief 统一推理提供者接口
/// @details ChatSession 依赖此接口而非具体 Backend/Agent
class WORKX_API ICompletionProvider {
   public:
    virtual ~ICompletionProvider() = default;

    /// @brief 提交推理请求
    /// @param request 推理请求参数
    /// @return 流式读取器（shared_ptr 以支持跨线程共享生命周期），失败时返回 nullptr
    virtual std::shared_ptr<IStreamReader> submit_completion(const CompletionRequest& request) = 0;

    /// @brief 中断当前推理
    virtual void interrupt() = 0;

    /// @brief 是否正在生成
    virtual bool is_generating() const = 0;
};

}  // namespace agent
