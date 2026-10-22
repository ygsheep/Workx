/**
 * @file context_compressor.cpp
 * @brief ContextCompressor 实现
 * @details 上下文压缩策略实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/compact/context_compressor.h"
#include "agent/compact/token_count.h"
#include "liblogger/logger.h"

#include <algorithm>

namespace agent {

// ============================================================
// summarize_tool_result — 将旧 tool_result 替换为摘要
// ============================================================

ChatMessage ContextCompressor::summarize_tool_result(const ChatMessage& msg) {
    // 摘要策略：保留工具名 + 结果前 100 字符 + 是否错误
    // 例如：[Result of Read: File content: ...]
    std::string summary = "[Result of " + msg.tool_name + ": ";
    const size_t preview_len = 100;
    if (msg.content.length() <= preview_len) {
        summary += msg.content;
    } else {
        summary += msg.content.substr(0, preview_len) + "...";
    }
    summary += "]";

    ChatMessage summarized = msg;
    summarized.content = std::move(summary);
    // 保留 is_error 标记：压缩不应丢失工具失败状态，否则模型会误以为之前的工具调用成功
    summarized.is_error = msg.is_error;
    return summarized;
}

// ============================================================
// compress — 压缩消息列表
// ============================================================

std::vector<ChatMessage> ContextCompressor::compress(
    const std::vector<ChatMessage>& messages) const {
    if (messages.empty()) return messages;

    std::vector<ChatMessage> result = messages;

    // 1. 消息数超限：保留最近 max_messages 条
    //    注意：保留尾部，丢弃头部（头部通常是旧对话）
    if (static_cast<int>(result.size()) > m_config.max_messages) {
        const size_t drop = result.size() - m_config.max_messages;
        result.erase(result.begin(), result.begin() + drop);
        LOG_INFO("[context_compressor] dropped {} old messages, remaining={}", drop, result.size());
    }

    // 2. token 估算超限：压缩旧的 tool_result
    if (m_config.compress_old_tools) {
        const int32_t estimated_tokens = compact::estimate_messages_tokens(result);
        if (estimated_tokens > m_config.max_tokens_estimate) {
            // 保留最近 N 条消息不压缩（N = 最近的完整一轮对话）
            // 简单策略：保留最近 10 条消息（含 user/assistant/tool_result）
            const size_t preserve_recent = std::min<size_t>(10, result.size());
            const size_t compress_end = result.size() - preserve_recent;

            size_t compressed_count = 0;
            for (size_t i = 0; i < compress_end; ++i) {
                if (result[i].role == ChatMessage::Role::Tool) {
                    result[i] = summarize_tool_result(result[i]);
                    ++compressed_count;
                }
            }

            if (compressed_count > 0) {
                const int32_t new_tokens = compact::estimate_messages_tokens(result);
                LOG_INFO(
                    "[context_compressor] compressed {} tool_results, "
                    "tokens {} -> {}",
                    compressed_count, estimated_tokens, new_tokens);
            }
        }
    }

    return result;
}

}  // namespace agent
