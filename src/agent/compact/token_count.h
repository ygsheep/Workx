/**
 * @file token_count.h
 * @brief Token 估算
 * @details 估算文本和消息序列的 token 数量。
 *          采用与 claude-code services/tokenEstimation.ts 一致的启发式：
 *          - 默认 chars / 4（英文/代码）
 *          - JSON/JSONL/JSONC chars / 2（更紧凑）
 *          - 图片/PDF 固定 2000 tokens
 *          未来可替换为 tiktoken 精确计算，调用方接口不变。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "agent/api/chat_types.h"

namespace agent::compact {

/// @brief 默认每 token 字节数（英文/代码）
constexpr int32_t BYTES_PER_TOKEN_DEFAULT = 4;

/// @brief JSON 类内容每 token 字节数（更紧凑）
constexpr int32_t BYTES_PER_TOKEN_JSON = 2;

/// @brief 图片/PDF 固定估算 token 数
constexpr int32_t IMAGE_TOKEN_ESTIMATE = 2'000;

/// @brief 按文件扩展名获取每 token 字节数
/// @details JSON/JSONL/JSONC 用 2，其它用 4
int32_t bytes_per_token_for_ext(std::string_view ext);

/// @brief 粗略估算文本 token 数
/// @param text 待估算文本
/// @param bytes_per_token 每 token 字节数（默认 4）
/// @return 估算 token 数（>=0）
int32_t rough_token_count(std::string_view text, int32_t bytes_per_token = BYTES_PER_TOKEN_DEFAULT);

/// @brief 估算单条 ChatMessage 的 token 数
/// @details 递归 content / reasoning_content / tool_uses / tool_call_id 等字段
int32_t estimate_message_tokens(const ChatMessage& msg);

/// @brief 估算消息序列的 token 数
/// @details 累加每条消息，并附加对话分隔符开销（每条 +4 tokens，对齐 claude-code）
int32_t estimate_messages_tokens(const std::vector<ChatMessage>& messages);

}  // namespace agent::compact
