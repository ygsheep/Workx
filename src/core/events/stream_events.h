/**
 * @file stream_events.h
 * @brief 流式事件类型（H-10：从 events.h 按域拆分）
 * @details 流式推理相关事件：用户输入、中断、Token 流、单步/会话级完成、错误。
 *          订阅方按需 include 本文件，避免引入系统/Agent 事件。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace agent {

// ============================================================
// 用户事件（TUI → Session）
// ============================================================
/// @brief 用户提交的原始文本（不含尾部换行）
/// @details is_local_command=true 表示该输入是本地 slash 命令（如 /help、/clear），
///          不会发送给 LLM，订阅方应跳过 token 统计累加。
struct UserInputEvent {
    std::string text;
    bool is_local_command = false;  ///< 是否为本地命令（不发送给 LLM）
};

/// @brief 中断请求（Ctrl+C）
struct InterruptEvent {
    bool force = false;  ///< 双击 Ctrl+C 时 force=true
};

// ============================================================
// 流式事件（Backend/Agent → TUI）
// ============================================================

/// @brief 单个流式 token
struct StreamTokenEvent {
    std::string session_id;
    std::string content_delta;
    std::string reasoning_delta;
    bool is_thinking = false;
    int32_t token_count = 0;
};

/// @brief Prompt 处理进度
struct StreamProgressEvent {
    std::string session_id;
    int32_t total = 0;
    int32_t processed = 0;
};

/// @brief 流式完成（整个会话/本轮推理结束）
/// @details 语义：LLM 推理完全结束（含 ReAct 多步全部完成），
///          UI 应执行完整结束流程：token 统计、状态转 IDLE、光标复位等。
///          注意：ReAct 循环中单步 LLM 输出结束（仍有 tool_use 待执行）
///          应发布 StepDoneEvent 而非本事件，避免触发会话级结束动作。
///          is_local_command=true 表示该事件源自本地命令输出（如 /help），
///          订阅方应跳过 token 统计累加。
struct StreamDoneEvent {
    std::string session_id;
    std::string full_content;
    std::string full_reasoning;
    bool was_interrupted = false;
    bool is_local_command = false;  ///< 是否为本地命令输出（不累加 token 统计）
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    // 上下文管理：Anthropic cache usage（OpenAI adapter 留 0）
    // 命中 cache 时 prompt_tokens 不含 cache 部分，需单独累加
    int32_t cache_creation_input_tokens = 0;
    int32_t cache_read_input_tokens = 0;
    // 上下文管理：DeepSeek 硬盘缓存命中（Anthropic adapter 留 0）
    int32_t prompt_cache_hit_tokens = 0;   ///< DeepSeek usage.prompt_cache_hit_tokens
    int32_t prompt_cache_miss_tokens = 0;  ///< DeepSeek usage.prompt_cache_miss_tokens
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
    double reasoning_ms = 0.0;  ///< 思考阶段实际耗时（毫秒，会话持久化/恢复用）
};

/// @brief 单步 LLM 输出结束（P3 新增，清理 StreamDoneEvent 语义污染）
/// @details 语义：ReAct 循环中一轮 LLM 流式输出结束，但仍有 tool_use 待执行，
///          后续还会有更多 LLM 调用。UI 应执行轻量收尾（spinner 停止、
///          formatter flush），但不应触发会话级结束动作（token 统计、
///          状态转 IDLE、光标复位）。
struct StepDoneEvent {
    std::string session_id;
    std::string full_content;    ///< 本步 LLM 输出的完整正文
    std::string full_reasoning;  ///< 本步 LLM 输出的完整推理内容
    double generation_ms = 0.0;  ///< 本步生成耗时（用于思考标记显示）
};

/// @brief 推理错误
struct StreamErrorEvent {
    std::string session_id;
    std::string message;
    bool retryable = false;
};

}  // namespace agent
