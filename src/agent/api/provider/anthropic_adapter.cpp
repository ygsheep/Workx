/**
 * @file anthropic_adapter.cpp
 * @brief Anthropic 协议适配器实现
 * @details Anthropic Messages API:
 *   - URL: POST /v1/messages
 *   - Auth: x-api-key header
 *   - Body: {model, max_tokens, stream, system?, messages}
 *   - SSE: named events (content_block_start/delta/stop, message_delta/stop)
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/api/provider/anthropic_adapter.h"

#include <nlohmann/json.hpp>

namespace agent {

std::string AnthropicAdapter::build_url(const std::string& base_url) const {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    if (!url.ends_with("/v1/messages")) {
        url += "/v1/messages";
    }
    return url;
}

std::vector<std::pair<std::string, std::string>> AnthropicAdapter::build_headers(
    const std::string& api_key) const {
    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    if (!api_key.empty()) {
        headers.emplace_back("x-api-key", api_key);
    }
    // anthropic-version 升级到 2025-01-01 以支持 thinking content block
    // （reasoning_content 结构化输出依赖此版本）
    headers.emplace_back("anthropic-version", "2025-01-01");
    return headers;
}

std::string AnthropicAdapter::build_request_body(const CompletionRequest& request,
                                                 const std::string& model_name) const {
    nlohmann::json j;
    j["model"] = model_name;
    j["stream"] = request.stream;
    // Anthropic 的 max_tokens 为必填字段
    j["max_tokens"] = request.max_tokens > 0 ? request.max_tokens : 4096;

    // 分离 system prompt 和 messages
    // Anthropic API 要求 system 为顶层字段，messages 只含 user/assistant
    std::string system_prompt;
    nlohmann::json messages = nlohmann::json::array();
    // 累积连续 tool_result，最终合并到同一 user 消息的 content 数组
    // （Anthropic 规范：连续 tool_result 必须放在同一 user 消息内）
    nlohmann::json pending_tool_results = nlohmann::json::array();

    auto flush_tool_results = [&]() {
        if (!pending_tool_results.empty()) {
            nlohmann::json m;
            m["role"] = "user";
            m["content"] = std::move(pending_tool_results);
            messages.push_back(std::move(m));
            pending_tool_results = nlohmann::json::array();
        }
    };

    for (const auto& msg : request.messages) {
        // System：多条 system_prompt 拼接（用空行分隔，不再取最后一条覆盖）
        if (msg.role == ChatMessage::Role::System) {
            if (!system_prompt.empty()) {
                system_prompt += "\n\n";
            }
            system_prompt += msg.content;
            continue;
        }

        // Tool：累积到 pending_tool_results，遇到非 Tool 消息时再 flush
        if (msg.role == ChatMessage::Role::Tool) {
            nlohmann::json tr = {{"type", "tool_result"},
                                 {"tool_use_id", msg.tool_call_id},
                                 {"content", msg.content}};
            // 工具执行失败时显式标记 is_error（对齐 Anthropic API 语义，
            // 让模型明确感知错误以便决策重试 / 换工具 / 终止）
            if (msg.is_error) {
                tr["is_error"] = true;
            }
            pending_tool_results.push_back(std::move(tr));
            continue;
        }

        // User / Assistant：先 flush 累积的 tool_results
        flush_tool_results();

        nlohmann::json m;

        if (msg.role == ChatMessage::Role::User) {
            m["role"] = "user";
            m["content"] = msg.content;
            messages.push_back(std::move(m));
            continue;
        }

        if (msg.role == ChatMessage::Role::Assistant) {
            m["role"] = "assistant";
            // assistant 消息统一用 content blocks 数组结构化输出
            // 区分 thinking（reasoning_content）和 text，避免思考被拼进回复
            nlohmann::json content_blocks = nlohmann::json::array();

            // thinking block（若有 reasoning_content，依赖 anthropic-version 2025-01-01+）
            if (!msg.reasoning_content.empty()) {
                content_blocks.push_back(
                    {{"type", "thinking"}, {"thinking", msg.reasoning_content}});
            }

            // text block
            if (!msg.content.empty()) {
                content_blocks.push_back({{"type", "text"}, {"text", msg.content}});
            }

            // tool_use blocks
            for (const auto& tu : msg.tool_uses) {
                content_blocks.push_back(
                    {{"type", "tool_use"},
                     {"id", tu.id},
                     {"name", tu.name},
                     {"input", tu.input.is_null() ? nlohmann::json::object() : tu.input}});
            }

            m["content"] = std::move(content_blocks);
            messages.push_back(std::move(m));
            continue;
        }

        // 其他角色（默认）跳过
    }

    // 循环结束后 flush 残留的 tool_results
    flush_tool_results();

    if (!system_prompt.empty()) {
        j["system"] = system_prompt;
    }
    j["messages"] = std::move(messages);

    // 工具 schema（function calling）
    if (request.has_tools()) {
        j["tools"] = request.tools;
    }

    if (request.temperature >= 0) {
        j["temperature"] = request.temperature;
    }
    if (request.top_p > 0) {
        j["top_p"] = request.top_p;
    }
    if (!request.stop_words.empty()) {
        j["stop"] = request.stop_words;
    }

    return j.dump();
}

bool AnthropicAdapter::parse_sse_event(const std::string& event_type, const std::string& data,
                                       StreamChunk& out) const {
    try {
        auto json_obj = nlohmann::json::parse(data);

        if (event_type == "content_block_delta") {
            // 内容增量或 tool_use input 增量
            // 用 .value() 防御性访问，避免缺字段时抛 out_of_range
            const auto& delta = json_obj.value("delta", nlohmann::json::object());
            const std::string delta_type = delta.value("type", "");
            if (delta_type == "input_json_delta") {
                // tool_use input JSON 增量
                out.is_tool_use_delta = true;
                out.tool_input_delta = delta.value("partial_json", "");
                return true;
            }
            if (delta.contains("text") && !delta["text"].is_null()) {
                out.content_delta = delta["text"].get<std::string>();
                return true;
            }
        } else if (event_type == "content_block_start") {
            // 内容块开始：tool_use 或 text
            const auto& block = json_obj.value("content_block", nlohmann::json::object());
            const std::string block_type = block.value("type", "text");
            if (block_type == "tool_use") {
                // tool_use 块开始
                out.is_tool_use_start = true;
                out.tool_use_id = block.value("id", "");
                out.tool_name = block.value("name", "");
                return true;
            }
            // 文本块可能有初始内容
            if (block.contains("text") && !block["text"].is_null()) {
                out.content_delta = block["text"].get<std::string>();
                return !out.content_delta.empty();
            }
        } else if (event_type == "message_start") {
            // message_start：携带 input_tokens 初始值（Anthropic 用 input_tokens 而非
            // prompt_tokens）
            if (json_obj.contains("message") && !json_obj["message"].is_null()) {
                const auto& message = json_obj.value("message", nlohmann::json::object());
                // 记录 response_id（用于并行 tool_use 拆分识别）
                if (message.contains("id") && !message["id"].is_null()) {
                    out.response_id = message["id"].get<std::string>();
                }
                if (message.contains("usage") && !message["usage"].is_null()) {
                    const auto& usage = message.value("usage", nlohmann::json::object());
                    out.prompt_tokens = usage.value("input_tokens", 0);
                    // 上下文管理：cache usage（prompt cache 命中时 input_tokens 不含 cache 部分）
                    out.cache_creation_input_tokens = usage.value("cache_creation_input_tokens", 0);
                    out.cache_read_input_tokens = usage.value("cache_read_input_tokens", 0);
                }
            }
            return false;  // 不算有效 chunk，仅记录 input_tokens
        } else if (event_type == "message_delta") {
            // 消息完成增量：stop_reason + usage
            const auto& delta = json_obj.value("delta", nlohmann::json::object());
            if (delta.contains("stop_reason") && !delta["stop_reason"].is_null()) {
                auto stop_reason = delta["stop_reason"].get<std::string>();
                if (stop_reason == "end_turn" || stop_reason == "max_tokens" ||
                    stop_reason == "stop_sequence" || stop_reason == "tool_use") {
                    out.is_final = true;
                    // usage 信息（包含 input_tokens 和 output_tokens）
                    if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
                        const auto& usage = json_obj.value("usage", nlohmann::json::object());
                        out.generated_tokens = usage.value("output_tokens", 0);
                        // 新增：input_tokens（Anthropic 用 input_tokens 而非 prompt_tokens）
                        out.prompt_tokens = usage.value("input_tokens", 0);
                        // 上下文管理：message_delta 的 usage 可能携带最终 cache 字段
                        // （Anthropic 在 message_delta 中重新发布完整 usage）
                        if (usage.contains("cache_creation_input_tokens")) {
                            out.cache_creation_input_tokens =
                                usage.value("cache_creation_input_tokens", 0);
                        }
                        if (usage.contains("cache_read_input_tokens")) {
                            out.cache_read_input_tokens = usage.value("cache_read_input_tokens", 0);
                        }
                    }
                    return true;
                }
            }
        } else if (event_type == "message_stop") {
            // 消息停止标记
            out.is_final = true;
            return true;
        }

        // content_block_stop 等其他事件忽略
        return false;

    } catch (const nlohmann::json::parse_error&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<ModelInfo> AnthropicAdapter::get_builtin_models(const std::string& base_url) const {
    // Anthropic 无公开 list_models 端点，返回内置模型列表
    // 根据 base_url 区分：DeepSeek 的 Anthropic 兼容端点返回 DeepSeek 模型
    // 注意：Anthropic 发布新模型时需手动更新 Claude 列表
    // 最后更新：2026-07-21

    // DeepSeek Anthropic 兼容端点
    if (base_url.find("deepseek.com") != std::string::npos) {
        return {
            {.name = "deepseek-v4-pro",
             .description = "DeepSeek V4 Pro (Anthropic 兼容端点)",
             .context_length = 128000},
            {.name = "deepseek-v4-flash",
             .description = "DeepSeek V4 Flash (Anthropic 兼容端点)",
             .context_length = 128000},
        };
    }

    // 原生 Anthropic Claude 模型列表
    return {
        // 2025-11 旗舰编码模型
        {.name = "claude-opus-4-5-20251101",
         .description = "Anthropic Claude Opus 4.5 (旗舰编码/Agent)",
         .context_length = 200000},
        // 2025-09 Sonnet 4.5（1M 上下文需 beta 头申请）
        {.name = "claude-sonnet-4-5-20250929",
         .description = "Anthropic Claude Sonnet 4.5 (1M beta)",
         .context_length = 200000},
        // 2025-10 Haiku 4.5 轻量快速
        {.name = "claude-haiku-4-5-20251001",
         .description = "Anthropic Claude Haiku 4.5 (轻量快速)",
         .context_length = 200000},
        // 2025-08 Opus 4.1 上一代旗舰
        {.name = "claude-opus-4-1-20250805",
         .description = "Anthropic Claude Opus 4.1 (上一代旗舰)",
         .context_length = 200000},
    };
}

}  // namespace agent
