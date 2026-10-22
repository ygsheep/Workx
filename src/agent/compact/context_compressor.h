/**
 * @file context_compressor.h
 * @brief ContextCompressor — 上下文压缩器（DEPRECATED）
 * @details 3.3：在 build_request 前压缩消息历史，避免长对话撑爆上下文窗口。
 *          压缩策略：
 *          1. 保留最近 N 轮 Thought/Action/Observation（完整保留）
 *          2. 更早的 tool_result 替换为摘要
 *          3. 超过 max_messages 的旧消息丢弃（仅保留最近 user message）
 *          4. 精确 token 估算使用 compact::estimate_messages_tokens
 *
 * @deprecated DS_CACHE_OPTIMIZATION_PLAN 层次 3 后此实现为缓存杀手：
 *             - erase(result.begin()) 删头部 → 前缀彻底失效
 *             - summarize_tool_result 原地改写 → 中段前缀失效
 *             - 阈值 max_messages=50 / max_tokens=8000 与真实窗口脱节
 *             新代码应使用 CacheAwareCompactor（cache_aware_compactor.h）。
 *             本文件仅保留以兼容历史测试，不再在 ReActLoop 中使用。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <vector>
#include "agent/api/chat_types.h"

namespace agent {

/// @brief 上下文压缩器（DEPRECATED，缓存杀手）
/// @details 无状态（仅配置），线程安全
/// @deprecated 使用 CacheAwareCompactor 替代
class [[deprecated(
    "Use CacheAwareCompactor instead (cache_aware_compactor.h)")]] ContextCompressor {
   public:
    /// @brief 压缩配置
    struct Config {
        int max_messages = 50;           ///< 最大保留消息数（超出则丢弃最旧消息）
        int max_tokens_estimate = 8000;  ///< 估计 token 上限（超出则压缩旧 tool_result）
        bool compress_old_tools = true;  ///< 是否压缩旧的 tool_result 为摘要
    };

    explicit ContextCompressor(Config cfg) : m_config(std::move(cfg)) {}

    ContextCompressor() : ContextCompressor(Config{}) {}

    /// @brief 压缩消息列表，返回压缩后的列表（不修改原始列表）
    /// @param messages 原始消息列表
    /// @return 压缩后的消息列表（长度 <= max_messages）
    [[nodiscard]] std::vector<ChatMessage> compress(const std::vector<ChatMessage>& messages) const;

    /// @brief 获取配置
    [[nodiscard]] const Config& config() const noexcept { return m_config; }

   private:
    Config m_config;

    /// @brief 将旧 tool_result 消息替换为摘要
    /// @param msg 原始 tool_result 消息
    /// @return 摘要消息
    [[nodiscard]] static ChatMessage summarize_tool_result(const ChatMessage& msg);
};

}  // namespace agent
