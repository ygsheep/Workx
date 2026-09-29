/**
 * @file headless.cpp
 * @brief Headless（非交互）执行模式实现（Issue #77）
 */

#include "agent/headless/headless.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>

#include "agent/factory.h"
#include "agent/model/provider_preset.h"
#include "agent/model/provider_config.h"   // load_provider_configs
#include "agent/config/app_config.h"
#include "agent/core/react_loop.h"
#include "agent/api/chat_types.h"
#include "agent/tool/context.h"
#include "agent/tool/registry.h"
#include "core/config/i_config_manager.h"
#include "core/task/task_manager.h"
#include "core/events/event_bus.h"
#include "core/utils/result_v2.h"
#include "core/utils/uuid.h"              // core::util::generate_uuid

namespace agent {

namespace {

/// @brief 将权限模式字符串映射到 PermissionMode（headless 无人值守档）
/// @return 成功返回 PermissionMode；失败返回空 optional
std::optional<tool::PermissionMode> parse_permission_mode(const std::string& s) {
    if (s.empty() || s == "default") return tool::PermissionMode::Default;
    if (s == "accept-edits") return tool::PermissionMode::AcceptEdits;
    if (s == "bypass-permissions") return tool::PermissionMode::BypassPermissions;
    if (s == "plan") return tool::PermissionMode::Plan;
    return std::nullopt;
}

/// @brief 序列化 ReActResult 为 text 输出（最终消息，失败回退部分内容）
std::string result_text(const ReActResult& r) {
    if (!r.final_answer.empty()) return r.final_answer;
    if (!r.partial_content.empty()) return r.partial_content;
    if (r.was_error && !r.error_message.empty()) return r.error_message;
    return "";
}

/// @brief 序列化 ReActResult 为 json 输出（最终消息 + 统计）
nlohmann::json result_json(const ReActResult& r, const std::string& session_id) {
    nlohmann::json usage = {
        {"prompt_tokens", r.prompt_tokens},
        {"generated_tokens", r.generated_tokens},
        {"total_tool_calls", r.total_tool_calls},
        {"total_iterations", r.total_iterations},
        {"duration_ms", r.total_duration_ms},
    };
    nlohmann::json j = {
        {"result", result_text(r)},
        {"session_id", session_id},
        {"usage", usage},
        {"goal_status", static_cast<int>(r.goal_status)},
        {"was_error", r.was_error},
        {"was_interrupted", r.was_interrupted},
        {"error_message", r.error_message},
    };
    return j;
}

/// @brief 将 ReActStep 序列化为 stream-json 的一行（NDJSON）
nlohmann::json step_json(const ReActStep& s) {
    nlohmann::json j = {
        {"type", [&s]() -> std::string {
            switch (s.type) {
                case ReActStepType::Thought: return "thought";
                case ReActStepType::Action: return "action";
                case ReActStepType::Observation: return "observation";
                case ReActStepType::FinalAnswer: return "final_answer";
            }
            return "unknown";
        }()},
        {"step_number", s.step_number},
    };
    if (s.type == ReActStepType::Thought) {
        j["text"] = s.thought_text;
    } else if (s.type == ReActStepType::Action) {
        j["tool_name"] = s.tool_name;
        j["tool_input"] = s.tool_input;
    } else if (s.type == ReActStepType::Observation) {
        j["observation"] = s.observation;
        j["is_error"] = s.is_error;
    } else if (s.type == ReActStepType::FinalAnswer) {
        j["text"] = s.thought_text;
    }
    return j;
}

} // namespace

HeadlessResult run_headless(IConfigManager& cfg,
                            ITaskManager& task_manager,
                            IEventBus& event_bus,
                            const HeadlessOptions& opts) {
    HeadlessResult result;

    // ---- 1. 解析 provider → 创建后端 ----
    const std::string provider = cfg.get_or<std::string>(keys::PROVIDER, "");
    const ProviderPreset* preset = provider.empty() ? nullptr : find_preset(provider);
    auto backend_result = create_backend(cfg, preset, event_bus);

    // 兜底：自定义条目（无 preset 默认 URL）——与 create_session 对齐
    if (!backend_result.provider) {
        const std::string active = cfg.get_or<std::string>(keys::PROVIDER, "");
        if (!active.empty()) {
            for (const auto& e : load_provider_configs(cfg)) {
                if (e.id == active || e.name == active) {
                    auto entry_result = create_backend_for_entry(cfg, e, event_bus);
                    if (entry_result.provider) backend_result = std::move(entry_result);
                    break;
                }
            }
        }
    }

    if (backend_result.remote_url.empty() || !backend_result.provider) {
        result.exit_code = 2;  // 参数/配置错误
        result.output = "error: 无法创建后端（缺少 remote_url / provider 配置）\n";
        return result;
    }

    // ---- 2. 注册内置工具 ----
    auto tool_registry = std::make_shared<tool::ToolRegistry>();
    // MCP：headless 下也尝试连接（与 create_session 一致；空 manager 则 MCP 工具返回"未连接"）
    std::shared_ptr<mcp::McpClientManager> mcp_manager;
    register_builtin_tools(*tool_registry, mcp_manager);

    // ---- 3. 系统提示词 ----
    const std::string user_prompt = cfg.get_or<std::string>(keys::SYSTEM_PROMPT, "");
    const std::string sys_prompt = build_system_prompt(user_prompt, *tool_registry);

    // ---- 4. 构造 ReActLoop（同步 run）----
    std::string session_id = core::util::generate_uuid();
    ReActLoop::Config loop_config;
    loop_config.max_iterations = cfg.get_or<int>(keys::AGENT_MAX_ITERATIONS, 40);

    std::string cwd = std::filesystem::current_path().string();

    ReActLoop loop(backend_result.provider.get(),
                   tool_registry,
                   loop_config,
                   &cfg,
                   &task_manager,
                   cwd,
                   /*external_compactor=*/nullptr,
                   /*event_bus=*/nullptr,   // #77 headless：不注入 event_bus，AskUserTool
                                            // 调用 ctx.event_bus() 抛错 → 自动拒绝提问
                   /*touch_collector=*/nullptr,
                   /*file_index_invalidator=*/nullptr,
                   session_id);

    // 权限模式（headless 无人值守档）
    auto pm = parse_permission_mode(opts.permission_mode);
    if (!pm) {
        result.exit_code = 2;
        result.output = "error: 未知权限模式 '" + opts.permission_mode +
                        "'（可选 default / accept-edits / bypass-permissions）\n";
        return result;
    }
    loop.set_permission_mode(*pm);

    // ---- 5. 组装消息 + tools_schema + 取消信号 ----
    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user(opts.task));

    nlohmann::json tools_schema = tool_registry->get_all_schemas();
    std::atomic<bool> should_cancel{false};

    // 步骤回调：stream-json 模式需要逐 step 输出
    ReActLoop::StepCallback on_step = nullptr;
    if (opts.output_format == "stream-json") {
        on_step = [&result](const ReActStep& step) {
            result.output += step_json(step).dump() + "\n";
        };
    }

    // ---- 6. 同步执行 ----
    ReActResult react_result;
    if (opts.output_format == "stream-json") {
        react_result = loop.run(messages, sys_prompt, tools_schema, should_cancel,
                                std::move(on_step), /*on_token=*/nullptr);
    } else {
        react_result = loop.run(messages, sys_prompt, tools_schema, should_cancel,
                                /*on_step=*/nullptr, /*on_token=*/nullptr);
    }

    // ---- 7. 序列化输出 + 退出码 ----
    if (opts.output_format == "json") {
        result.output = result_json(react_result, session_id).dump(2) + "\n";
    } else if (opts.output_format == "stream-json") {
        // 已逐 step 累积在 result.output，末尾补最终结果一行
        result.output += result_json(react_result, session_id).dump() + "\n";
    } else {
        // text
        result.output = result_text(react_result);
        if (result.output.empty()) {
            result.output = react_result.error_message;
        }
        result.output += "\n";
    }

    // 退出码语义：0 成功 / 1 任务失败 / 3 预算中断
    if (react_result.was_error) {
        result.exit_code = 1;
    } else if (react_result.was_interrupted) {
        result.exit_code = 1;
    } else {
        result.exit_code = 0;
    }

    return result;
}

} // namespace agent
