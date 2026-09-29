/**
 * @file chat_types.h
 * @brief 聊天相关数据类型
 * @details ChatMessage, CompletionRequest, StreamChunk
 *          BackendConfig/ModelInfo 已拆分到 backend_types.h（P1），本文件 #include 作为 shim
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>
#include "agent/model/provider_type.h"
#include "agent/api/backend_types.h"

namespace agent {

// ============================================================
// ToolUse（Assistant 消息中的工具调用块）
// ============================================================

/// @brief 工具调用（对应 Anthropic tool_use content block）
struct ToolUse {
    std::string id;        ///< 工具调用 ID（LLM 生成，如 "toolu_xxx"）
    std::string name;      ///< 工具名称（如 "Read" / "Write"）
    nlohmann::json input;  ///< 工具输入参数（JSON 对象）
};

// ============================================================
// ChatMessage
// ============================================================

/// @brief 聊天消息
struct ChatMessage {
    enum class Role { System, User, Assistant, Tool };

    Role role = Role::User;
    std::string content;
    std::string reasoning_content;   ///< 思考/推理内容（如 deepseek thinking）
    std::string tool_call_id;        ///< Tool 角色时的调用 ID（对应 tool_use_id）
    std::string tool_name;           ///< Tool 角色时的工具名
    std::vector<ToolUse> tool_uses;  ///< Assistant 角色时的工具调用列表
    bool is_error = false;  ///< Tool 角色时标记工具执行失败（对齐 Anthropic is_error）
    std::vector<std::string> image_paths;  ///< User 消息携带的图片绝对路径（多模态）
    double reasoning_ms = 0.0;  ///< 思考阶段耗时（毫秒，最后一轮 LLM 响应；会话持久化/恢复用）

    /// @brief 便捷构造
    static ChatMessage system(const std::string& text) {
        return {Role::System, text, {}, {}, {}, {}};
    }
    static ChatMessage user(const std::string& text) { return {Role::User, text, {}, {}, {}, {}}; }
    /// @brief 便捷构造（带图片附件，OpenAI 兼容多模态）
    static ChatMessage user(const std::string& text, std::vector<std::string> images) {
        ChatMessage msg = user(text);
        msg.image_paths = std::move(images);
        return msg;
    }
    static ChatMessage assistant(const std::string& text) {
        return {Role::Assistant, text, {}, {}, {}, {}};
    }

    /// @brief 构造 tool_result 消息（Tool 角色）
    /// @param tool_use_id 对应的 tool_use ID
    /// @param tool_name 工具名
    /// @param result_content 工具执行结果文本
    /// @param is_err 工具执行是否失败（true 时 Anthropic adapter 会输出 is_error 字段）
    static ChatMessage tool_result(const std::string& tool_use_id, const std::string& tool_name,
                                   const std::string& result_content, bool is_err = false) {
        ChatMessage msg;
        msg.role = Role::Tool;
        msg.tool_call_id = tool_use_id;
        msg.tool_name = tool_name;
        msg.content = result_content;
        msg.is_error = is_err;
        return msg;
    }
};

// ============================================================
// CompletionRequest
// ============================================================

/// @brief 推理请求
struct CompletionRequest {
    std::vector<ChatMessage> messages;
    std::vector<std::string> stop_words;
    nlohmann::json tools;     ///< 工具 schema 数组（function calling），空表示无工具
    int32_t max_tokens = -1;  ///< -1 表示不限制
    float temperature = 0.8f;
    float top_p = 0.95f;
    bool stream = true;  ///< 强制流式模式

    /// @brief 是否携带工具
    [[nodiscard]] bool has_tools() const {
        return !tools.is_null() && tools.is_array() && !tools.empty();
    }
};

// ============================================================
// StreamChunk
// ============================================================

/// @brief 流式响应的单个数据块
struct StreamChunk {
    std::string content_delta;    ///< 正文增量
    std::string reasoning_delta;  ///< 推理内容增量
    int32_t token_count = 0;      ///< 当前 token 数
    bool is_final = false;        ///< 是否为最终块

    // 以下字段仅在 is_final=true 时有效
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;

    // 上下文管理：Anthropic cache usage（OpenAI adapter 留 0）
    // 用于精确计算当前上下文 token 总量（与 claude-code tokenCountWithEstimation 对齐）
    int32_t cache_creation_input_tokens = 0;  ///< Anthropic cache_creation_input_tokens
    int32_t cache_read_input_tokens = 0;      ///< Anthropic cache_read_input_tokens
    std::string response_id;  ///< Anthropic message.id（并行 tool_use 拆分识别）

    // 上下文管理：DeepSeek 硬盘缓存命中（Anthropic adapter 留 0）
    // usage.prompt_cache_hit_tokens / prompt_cache_miss_tokens
    // 命中部分不计入 prompt_tokens，需单独累加用于命中率观测
    int32_t prompt_cache_hit_tokens = 0;   ///< DeepSeek usage.prompt_cache_hit_tokens
    int32_t prompt_cache_miss_tokens = 0;  ///< DeepSeek usage.prompt_cache_miss_tokens

    // tool_use 流式事件（Anthropic content_block_start/delta）
    bool is_tool_use_start = false;  ///< tool_use content_block 开始
    bool is_tool_use_delta = false;  ///< tool_use input JSON 增量
    std::string tool_use_id;         ///< is_tool_use_start 时填充
    std::string tool_name;           ///< is_tool_use_start 时填充
    std::string tool_input_delta;    ///< is_tool_use_delta 时填充（partial JSON）
};

// BackendConfig 和 ModelInfo 已移至 backend_types.h（P1 拆分）
// 本文件通过上方 #include "agent/api/backend_types.h" 提供向后兼容 shim

}  // namespace agent
