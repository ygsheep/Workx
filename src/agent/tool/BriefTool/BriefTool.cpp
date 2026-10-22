/**
 * @file BriefTool.cpp
 * @brief BriefTool — 强制用户通信通道实现（Issue #56 方案 B）
 * @details 复用 AskUser 的事件通道（AskUserRequestEvent + ChoicePanel 模态）。
 *          工作线程发布事件后阻塞等待 result_promise（含超时），用户确认/答复
 *          通过 ChoicePanel 回填。相比 AskUserTool 输入为单个 question + status：
 *          - status="proactive"（默认）：开工/临门一脚确认，需先征得用户同意再继续
 *          - status="normal"：普通简短询问，用户可为自定义输入
 * @version 1.0.0
 * @date 2026-09
 */

#include "agent/tool/BriefTool/BriefTool.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/events/i_event_bus.h"
#include "core/events/agent_events.h"
#include "core/utils/error.h"
#include "core/utils/result_v2.h"
#include "liblogger/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <future>
#include <memory>
#include <utility>

namespace agent::tool {

namespace {

/// @brief 默认超时时间（毫秒）= 5 分钟
constexpr int32_t DEFAULT_TIMEOUT_MS = 300000;

const std::string kToolName = "Brief";
const std::string kToolDesc =
    "High-guarantee, blocking user confirmation channel. Use when you MUST get an "
    "explicit user go-ahead before starting or continuing an action (work-begin "
    "confirmation, last-moment sign-off). Blocks until the user responds or timeout.";
const std::string kToolPrompt =
    "Use this tool when you need a mandatory user gate before proceeding. This is the "
    "ONLY allowed channel for user interaction during execution; never proceed past a "
    "decision point without user consent.\n"
    "Two modes via the 'status' field:\n"
    "- \"proactive\" (default): a hard confirmation gate. You MUST call this before "
    "performing any significant, irreversible, or user-facing action, and wait for the "
    "user's answer before continuing. Treat the returned answer as binding.\n"
    "- \"normal\": an ordinary brief question where the user may pick an option or type "
    "custom input.\n\n"
    "Usage notes:\n"
    "- Provide a single, clear 'question'; the model blocks until the user "
    "responds (or the optional timeout_ms elapses). Default timeout_ms=300000 (5min); "
    "set 0 to wait indefinitely. A timeout returns {\"status\":\"timeout\"}.\n"
    "- The user can always supply custom text; treat a custom answer as authoritative.\n"
    "- Output: {\"status\":\"submitted\"|\"cancelled\"|\"timeout\", \"answer\":\"...\"}.\n"
    "- Do NOT bypass this channel with reasoning-only filler; if a gate is required, "
    "call the tool and wait.";

/// @brief 小写化工具字符串（用于判断用户答语）
std::string to_lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

// ============================================================
// 元信息
// ============================================================

const std::string& BriefTool::name() const { return kToolName; }
const std::string& BriefTool::description() const { return kToolDesc; }
const std::string& BriefTool::prompt() const { return kToolPrompt; }

nlohmann::json BriefTool::input_schema() const {
    static const std::string schema_str = R"JSON({
        "type": "object",
        "properties": {
            "question": {
                "type": "string",
                "description": "The single, clear question for the user, ending with a question mark. The user sees this verbatim in the confirmation modal."
            },
            "status": {
                "type": "string",
                "enum": ["proactive", "normal"],
                "default": "proactive",
                "description": "proactive: mandatory go-ahead gate (user must confirm before you continue). normal: ordinary brief question."
            },
            "timeout_ms": {
                "type": "integer",
                "default": 300000,
                "description": "Timeout in milliseconds. 0 = no timeout (wait indefinitely). Default 300000."
            }
        },
        "required": ["question"]
    })JSON";
    return nlohmann::json::parse(schema_str);
}

// ============================================================
// call
// ============================================================

ResultV2<ToolResult> BriefTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 1. 校验输入
    if (!input.contains("question") || !input["question"].is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Brief: missing string field 'question'");
    }
    const std::string question = input["question"].get<std::string>();
    if (question.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Brief: 'question' must not be empty");
    }
    const std::string status = input.value("status", std::string("proactive"));
    const int32_t timeout_ms = input.value("timeout_ms", DEFAULT_TIMEOUT_MS);

    // 2. 无确认通道则 fail-closed
    if (!ctx.event_bus_ptr) {
        return ResultV2<ToolResult>::err(
            Error::Code::ToolExecutionFailed,
            "Brief: no user interaction channel (event bus) available");
    }

    // 3. 创建 promise/future 通道 + 取消标志
    auto promise = std::make_shared<std::promise<AskUserResult>>();
    std::future<AskUserResult> future = promise->get_future();
    std::shared_ptr<std::atomic<bool>> cancel_flag;
    if (timeout_ms > 0) {
        cancel_flag = std::make_shared<std::atomic<bool>>(false);
    }

    // 4. 发布 AskUserRequestEvent（异步入队，主循环 drain 后弹 ChoicePanel）
    //    契约与 permission_ask.cpp / AskUserTool 一致：questions 为 {questions:[...]}
    //    对象，options 为 {label, description} 对象数组；模态标题随 status 区分。
    const bool proactive = (to_lower_copy(status) == "proactive");
    AskUserRequestEvent evt{
        .session_id = ctx.session_id,
        .questions = nlohmann::json::object(
            {{"questions",
              nlohmann::json::array(
                  {{{"question", question},
                    {"header", proactive ? "确认一下(继续?)" : "Brief 询问"},
                    {"allow_custom_input", true},
                    {"options",
                     nlohmann::json::array({
                         {{"label", "确认"}, {"description", proactive ? "确认并继续" : "采纳"}},
                         {{"label", "取消"}, {"description", "中止本次动作"}},
                     })}}})}}),
        .timeout_ms = timeout_ms,
        .result_promise = promise,
        .cancel_flag = cancel_flag};
    ctx.event_bus_ptr->publish_async(evt);
    LOG_INFO("[BriefTool] published AskUserRequestEvent, status={}, timeout_ms={}", status,
             timeout_ms);

    // 5. 阻塞等待结果（支持超时）
    AskUserResult result;
    bool timed_out = false;

    if (timeout_ms > 0) {
        auto wait = future.wait_for(std::chrono::milliseconds(timeout_ms));
        if (wait == std::future_status::timeout) {
            timed_out = true;
            LOG_WARN("[BriefTool] timed out after {}ms", timeout_ms);
            if (cancel_flag) {
                cancel_flag->store(true, std::memory_order_release);
            }
            ctx.event_bus_ptr->publish_async(AskUserTimeoutEvent{.session_id = ctx.session_id});
        }
    } else {
        future.wait();
    }

    if (!timed_out) {
        try {
            result = future.get();
        } catch (const std::future_error&) {
            result.submitted = false;
            LOG_WARN("[BriefTool] future_error while getting result");
        }
    }

    // 6. 构造返回 JSON
    nlohmann::json out;
    if (timed_out) {
        out["status"] = "timeout";
        out["message"] = "User did not respond within " + std::to_string(timeout_ms / 1000) + "s";
    } else if (result.submitted && !result.answers.empty()) {
        out["status"] = "submitted";
        const auto& answer = result.answers.front().second;
        out["answer"] = answer;
        // proactive 下推断确认结果：确认/继续/yes 视为批准；normal 不引入门控语义
        if (proactive) {
            const std::string lower = to_lower_copy(answer);
            out["approved"] = (lower == "确认" || lower == "继续" || lower == "yes" ||
                               lower == "continue" || lower == "approve");
        }
    } else if (result.submitted) {
        out["status"] = "submitted";
        out["answer"] = "";
        if (proactive) out["approved"] = false;
    } else {
        out["status"] = "cancelled";
        out["message"] = "User cancelled the brief";
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(out));
}

}  // namespace agent::tool
