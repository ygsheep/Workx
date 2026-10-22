/**
 * @file AskUserTool.cpp
 * @brief AskUserTool — 向用户提问工具实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/AskUser/AskUserTool.h"
#include "agent/tool/AskUser/ask_user_config.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/events/i_event_bus.h"
#include "core/events/agent_events.h"
#include "core/utils/result_v2.h"
#include "core/utils/error.h"
#include "liblogger/logger.h"

#include <future>
#include <memory>
#include <chrono>
#include <atomic>

namespace agent::tool {

namespace {

/// @brief 默认超时时间（毫秒）= 5 分钟
constexpr int32_t DEFAULT_TIMEOUT_MS = 300000;

const std::string kToolName = "AskUser";
const std::string kToolDesc =
    "Asks the user multiple choice questions to gather information, clarify ambiguity, "
    "understand preferences, make decisions or offer them choices. "
    "Blocks until the user responds or timeout (default 5 minutes) is reached.";
const std::string kToolPrompt =
    "Use this tool when you need to ask the user questions during execution. "
    "This allows you to:\n"
    "1. Gather user preferences or requirements\n"
    "2. Clarify ambiguous instructions\n"
    "3. Get decisions on implementation choices as you work\n"
    "4. Offer choices to the user about what direction to take.\n\n"
    "Usage notes:\n"
    "- Users will always be able to provide custom text input\n"
    "- Use multiSelect: true to allow multiple answers to be selected for a question\n"
    "- If you recommend a specific option, make that the first option in the list "
    "and add \"(Recommended)\" at the end of the label\n"
    "- The tool blocks until the user responds or timeout (default 5 minutes) is reached";

}  // namespace

// ============================================================
// 元信息
// ============================================================

const std::string& AskUserTool::name() const { return kToolName; }
const std::string& AskUserTool::description() const { return kToolDesc; }
const std::string& AskUserTool::prompt() const { return kToolPrompt; }

nlohmann::json AskUserTool::input_schema() const {
    // 对齐 cc AskUserQuestionTool 的 schema 结构
    // 用 json::parse 解析原始字符串，避免深层嵌套初始化列表耗尽 MSVC 编译器堆
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {
            "questions": {
                "type": "array",
                "minItems": 1,
                "maxItems": 4,
                "description": "Questions to ask the user (1-4 questions)",
                "items": {
                    "type": "object",
                    "properties": {
                        "question": {
                            "type": "string",
                            "description": "The complete question to ask the user. Should be clear, specific, and end with a question mark."
                        },
                        "header": {
                            "type": "string",
                            "description": "Very short label displayed as a chip/tag (max 12 chars). Examples: 'Auth method', 'Library', 'Approach'."
                        },
                        "multiSelect": {
                            "type": "boolean",
                            "default": false,
                            "description": "Set to true to allow the user to select multiple options instead of just one."
                        },
                        "allow_custom_input": {
                            "type": "boolean",
                            "default": true,
                            "description": "Whether to append a custom-input option at the end. Default true."
                        },
                        "options": {
                            "type": "array",
                            "minItems": 2,
                            "maxItems": 4,
                            "description": "The available choices (2-4 options). Each option should be a distinct, mutually exclusive choice (unless multiSelect is enabled).",
                            "items": {
                                "type": "object",
                                "properties": {
                                    "label": {
                                        "type": "string",
                                        "description": "The display text for this option. Concise (1-5 words)."
                                    },
                                    "description": {
                                        "type": "string",
                                        "description": "Explanation of what this option means or what will happen if chosen."
                                    }
                                },
                                "required": ["label"]
                            }
                        }
                    },
                    "required": ["question", "header", "options"]
                }
            },
            "timeout_ms": {
                "type": "integer",
                "default": 300000,
                "description": "Timeout in milliseconds. 0 = no timeout (wait indefinitely). Default 300000 (5 minutes)."
            }
        },
        "required": ["questions"]
    })JSON";
    return nlohmann::json::parse(schema_str);
}

// ============================================================
// call
// ============================================================

ResultV2<ToolResult> AskUserTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    LOG_INFO("[AskUserTool] call() entered, has_questions={}", input.contains("questions"));
    // 1. 校验输入基本结构
    if (!input.contains("questions") || !input["questions"].is_array()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "AskUser: missing 'questions' array");
    }
    if (input["questions"].empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "AskUser: 'questions' must not be empty");
    }

    // 2. 解析超时（默认 5 分钟）
    int32_t timeout_ms = input.value("timeout_ms", DEFAULT_TIMEOUT_MS);

    // 3. 校验 questions 结构（宿主无关校验，TUI 渲染时自行二次解析）
    if (!validate_ask_user_json(input)) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "AskUser: failed to parse 'questions' as choice config");
    }

    // 4. 创建 promise/future 通道 + 取消标志
    auto promise = std::make_shared<std::promise<AskUserResult>>();
    std::future<AskUserResult> future = promise->get_future();
    // cancel_flag：工作线程超时后置位，TUI 主循环检查后关闭 ChoicePanel。
    // timeout_ms > 0 时才需要取消机制；timeout_ms == 0（不限时）传 nullptr。
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    if (timeout_ms > 0) {
        cancel_flag = std::make_shared<std::atomic<bool>>(false);
    }

    // 5. 发布 AskUserRequestEvent（异步入队，主循环 drain 后弹 ChoicePanel）
    IEventBus& bus = ctx.event_bus();
    AskUserRequestEvent evt{
        .session_id = ctx.session_id,
        .questions = input,  // 完整 input 对象（含 questions 键），与 parse_choice_config 契约一致
        .timeout_ms = timeout_ms,
        .result_promise = promise,
        .cancel_flag = cancel_flag};
    bus.publish_async(evt);
    LOG_INFO("[AskUserTool] published AskUserRequestEvent, timeout_ms={}, waiting for TUI response",
             timeout_ms);

    // 6. 阻塞等待结果（支持超时）
    AskUserResult result;
    bool timed_out = false;

    LOG_INFO("[AskUserTool] waiting for TUI response, timeout_ms={}", timeout_ms);

    if (timeout_ms > 0) {
        auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
        if (status == std::future_status::timeout) {
            timed_out = true;
            LOG_WARN("[AskUserTool] timed out after {}ms", timeout_ms);
            // 置位取消标志并发布超时事件，唤醒 TUI 主循环关闭 ChoicePanel。
            // 必须先置位 cancel_flag 再发事件，确保 TUI 收到 KEY_WAKE 时能看到标志已置位。
            if (cancel_flag) {
                cancel_flag->store(true, std::memory_order_release);
            }
            bus.publish_async(AskUserTimeoutEvent{.session_id = ctx.session_id});
        }
        // ready 或 timeout 都尝试 get（timeout 时若 TUI 恰好回填也能取到）
        // 注意：若已 timeout 且 promise 未 set_value，get() 会抛异常，需保护
    } else {
        future.wait();
    }

    if (!timed_out) {
        try {
            result = future.get();
            LOG_INFO("[AskUserTool] got response, submitted={}", result.submitted);
        } catch (const std::future_error&) {
            // promise 已销毁或未设值，视为取消
            result.submitted = false;
            LOG_WARN("[AskUserTool] future_error while getting result");
        }
    } else {
        // 超时：构造 timeout 结果
        result.submitted = false;
    }

    // 7. 构造返回 JSON
    nlohmann::json out;
    if (timed_out) {
        out["status"] = "timeout";
    } else if (result.submitted) {
        out["status"] = "submitted";
        nlohmann::json ans = nlohmann::json::object();
        for (const auto& [q, a] : result.answers) {
            ans[q] = a;
        }
        out["answers"] = ans;
    } else {
        out["status"] = "cancelled";
    }

    // 附人类可读说明，便于 LLM 理解
    if (timed_out) {
        out["message"] = "User did not respond within " + std::to_string(timeout_ms / 1000) + "s";
    } else if (!result.submitted) {
        out["message"] = "User cancelled the question";
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(out));
}

}  // namespace agent::tool
