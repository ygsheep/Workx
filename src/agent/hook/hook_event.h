/**
 * @file hook_event.h
 * @brief 通用 Hook 事件系统 — 事件枚举 / 上下文负载 / 执行结果 / 注册定义
 * @details Issue #50。定义 8 个 Hook 事件、4 种 Hook 类型，以及执行器返回
 *          的 Promise 语义（blockingError / preventContinuation 对齐 cc）。
 * @version 1.0.1
 * @date 2026-08
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::hook {

// ============================================================
// Hook 事件枚举（8 个）
// ============================================================

enum class HookEvent {
    PreToolUse,        // 工具执行前：拦截 / 注入上下文
    PostToolUse,       // 工具执行后：记录 / 后续处理
    SessionStart,      // 会话开始：初始化
    SessionEnd,        // 会话结束：清理
    Stop,              // Agent 停止：阻塞错误 / preventContinuation
    SubagentStart,     // 子 Agent 开始：上下文注入
    SubagentStop,      // 子 Agent 停止：结果处理
    PermissionRequest  // 权限请求：动态授权
};

/// @brief 事件名转字符串（"PreToolUse" 等，frontmatter/config 一致）
const char* to_string(HookEvent event) noexcept;

/// @brief 字符串转事件（未知返回 std::nullopt → 调用方忽略）
std::optional<HookEvent> parse_event(const std::string& name) noexcept;

// ============================================================
// HookContext — 事件携带的上下文负载（按事件取用相应字段）
// ============================================================

struct HookContext {
    std::string session_id;  // 当前会话
    std::string cwd;         // 工作目录
    std::string request_id;  // turn 级请求 ID

    // PreToolUse / PostToolUse / PermissionRequest
    std::string tool_name;                                 // 工具名（Bash/Read/Write/...）
    nlohmann::json tool_input = nlohmann::json::object();  // 工具参数
    std::string tool_result;                               // PostToolUse：工具原始返回
    bool tool_error = false;                               // PostToolUse：是否出错

    // Stop / SubagentStop
    std::string final_answer;  // Agent 收尾答复
    std::string stop_reason;   // interrupted / error / completed / at_limit

    // SubagentStart / SubagentStop
    std::string subagent_id;  // 子代理 task_id
    std::string subagent_prompt;
};

// ============================================================
// HookResult — 执行器返回（Promise 语义，同 cc）
// ============================================================

struct HookResult {
    std::string message;                       // 注入用户可见信息
    std::optional<std::string> blockingError;  // 阻断错误（注入用户消息）
    bool preventContinuation = false;          // 阻止 query 循环继续
    std::string stopReason;                    // 附加 stop reason
    std::string output;                        // hook 自身输出（供注入上下文）
};

// ============================================================
// Hook 类型枚举（4 种）
// ============================================================

enum class HookType {
    Command,  // shell 命令
    Prompt,   // LLM prompt 评估
    Agent,    // agentic verifier
    Http      // POST 到 URL
};

/// @brief 类型名转字符串（"command" / "prompt" / "agent" / "http"）
const char* type_to_string(HookType type) noexcept;

// ============================================================
// HookDefinition — 注册定义（来自 config JSON / frontmatter 对象）
// ============================================================

struct HookDefinition {
    HookEvent event = HookEvent::PreToolUse;
    HookType type = HookType::Command;
    std::string match;  // if 条件（permission-rule 语法 "Bash(git *)"）

    // command
    std::string command;  // shell 命令

    // http
    std::string url;                                    // POST URL
    nlohmann::json headers = nlohmann::json::object();  // 自定义请求头
    std::vector<std::string> allowedEnvVars;            // 允许透传的 env

    // prompt / agent
    std::string prompt;  // LLM 提示
    std::string model;   // 指定模型
    std::string agent;   // agentic verifier

    int timeout_ms = 30000;      // 超时（毫秒）
    bool statusMessage = false;  // 显示状态信息
    bool once = false;           // 只运行一次
    bool async = false;          // 异步（不阻塞主线程）
    bool asyncRewake = false;    // 异步唤醒重置 once

    /// @brief 从 JSON 对象解析（字段均可选，缺失用默认值）
    static HookDefinition from_json(const nlohmann::json& obj);
};

}  // namespace agent::hook
