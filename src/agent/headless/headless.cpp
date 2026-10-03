/**
 * @file headless.cpp
 * @brief Headless（非交互）执行模式实现（Issue #77）
 */

#include "agent/headless/headless.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>

#include "agent/api/chat_types.h"
#include "agent/config/app_config.h"
#include "agent/core/goal_verdict.h"  // #126：parse_goal（--goal 合法性校验）
#include "agent/core/react_loop.h"
#include "agent/factory.h"
#include "agent/headless/headless_internal.h"  // #77：内部可测件声明（@internal）
#include "agent/util/git_checkpoint.h"         // #81：git 基线检查点（收尾改动清单）
#include "agent/model/provider_config.h"       // load_provider_configs
#include "agent/model/provider_preset.h"
#include "agent/tool/context.h"
#include "agent/tool/registry.h"
#include "core/config/i_config_manager.h"
#include "liblogger/logger.h"  // #126：--goal 覆盖生效时留一条可采集的证据
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "core/utils/result_v2.h"
#include "core/utils/uuid.h"  // core::util::generate_uuid

namespace agent {

// ============================================================================
// 内部可测件（声明见 headless_internal.h；@internal 仅供单元测试）
// ============================================================================

/// @brief 将权限模式字符串映射到 PermissionMode（headless 无人值守档）
/// @return 成功返回 PermissionMode；失败返回空 optional
std::optional<tool::PermissionMode> parse_permission_mode(const std::string& s) {
    if (s.empty() || s == "default") return tool::PermissionMode::Default;
    if (s == "accept-edits") return tool::PermissionMode::AcceptEdits;
    if (s == "bypass-permissions") return tool::PermissionMode::BypassPermissions;
    if (s == "plan") return tool::PermissionMode::Plan;
    // #85：严格档——无人值守时未命中白名单的命令因无确认通道而被拒，故对
    //      headless 是最有用的档位。取值集合须与 chat_session.cpp 的同构函数一致。
    if (s == "strict") return tool::PermissionMode::Strict;
    return std::nullopt;
}

/// @brief 序列化 ReActResult 为 text 输出（最终消息，失败回退部分内容）
std::string result_text(const ReActResult& r) {
    if (!r.final_answer.empty()) return r.final_answer;
    if (!r.partial_content.empty()) return r.partial_content;
    if (r.was_error && !r.error_message.empty()) return r.error_message;
    return "";
}

/// @brief 退出码语义：0 成功 / 1 任务失败或被中断
int derive_exit_code(const ReActResult& r) { return (r.was_error || r.was_interrupted) ? 1 : 0; }

namespace {

/// @brief #81：本次运行相对基线 commit 的改动摘要（非仓库或无改动时为空串）
/// @details 无人值守场景下回答"agent 到底改了什么"，供调用方归档与 review。
std::string git_diff_summary_text() {
    auto& checkpoint = util::GitCheckpoint::instance();
    if (!checkpoint.info().valid) return {};
    const auto summary = checkpoint.diff_since_base();
    if (summary.files.empty()) return {};
    return util::GitCheckpoint::format_summary(summary);
}

/// @brief 序列化 ReActResult 为 json 输出（最终消息 + 统计）
nlohmann::json result_json(const ReActResult& r, const std::string& session_id) {
    nlohmann::json usage = {
        {"prompt_tokens", r.prompt_tokens},       {"generated_tokens", r.generated_tokens},
        {"total_tool_calls", r.total_tool_calls}, {"total_iterations", r.total_iterations},
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
    // #81：收尾报告——本次运行相对基线 commit 的改动清单。
    //      非 git 仓库或无改动时不写入该字段，保持输出精简。
    const std::string diff_text = git_diff_summary_text();
    if (!diff_text.empty()) {
        j["git_diff_summary"] = diff_text;
    }
    return j;
}

/// @brief 将 ReActStep 序列化为 stream-json 的一行（NDJSON）
nlohmann::json step_json(const ReActStep& s) {
    nlohmann::json j = {
        {"type",
         [&s]() -> std::string {
             switch (s.type) {
                 case ReActStepType::Thought:
                     return "thought";
                 case ReActStepType::Action:
                     return "action";
                 case ReActStepType::Observation:
                     return "observation";
                 case ReActStepType::FinalAnswer:
                     return "final_answer";
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

/// @brief 解析 provider 配置并创建后端
/// @details 先按 preset 创建；失败则回退到自定义 provider 条目（与 create_session 对齐）
BackendCreateResult resolve_backend(IConfigManager& cfg, IEventBus& event_bus) {
    const std::string provider = cfg.get_or<std::string>(keys::PROVIDER, "");
    const ProviderPreset* preset = provider.empty() ? nullptr : find_preset(provider);
    auto backend = create_backend(cfg, preset, event_bus);
    if (backend.provider) return backend;

    for (const auto& e : load_provider_configs(cfg)) {
        if (e.id != provider && e.name != provider) continue;
        auto entry_result = create_backend_for_entry(cfg, e, event_bus);
        if (entry_result.provider) return entry_result;
        break;
    }
    return backend;
}

/// @brief 构造 headless 专用的 ReActLoop
/// @note 不注入 event_bus：AskUserTool 调用 ctx.event_bus() 抛错 → 自动拒绝提问，
///       避免无人值守时阻塞等待应答超时。
/// @note #77：形参由 BackendCreateResult& 改为裸 provider 指针，使
///       run_headless_with_provider 能注入测试后端而不必伪造 BackendCreateResult。
std::unique_ptr<ReActLoop> build_loop(IConfigManager& cfg, ITaskManager& task_manager,
                                      ICompletionProvider* provider,
                                      std::shared_ptr<tool::ToolRegistry> tool_registry,
                                      const std::string& session_id) {
    ReActLoop::Config loop_config;
    loop_config.max_iterations = cfg.get_or<int>(keys::AGENT_MAX_ITERATIONS, 40);
    // Issue #78 阶段 P2：headless 是评测入口，验证门禁在这里**默认开启**。
    // 必须在此显式接线 —— headless 自己 new ReActLoop，既不经过 chat_session 的
    // goal 注入，也不经过 GoalGuardedAgent 包壳，漏了这条评测链路就永远零验证。
    // 默认开启本身是安全的：未声明 agent.goal 时目标按项目线索推断，
    // 探测不到可跑命令则 goal.type == None，门禁自动放行。
    const std::string cwd = std::filesystem::current_path().string();
    agent::apply_verification_gate(loop_config, cfg, /*enabled_by_default=*/true, cwd);

    return std::make_unique<ReActLoop>(provider, tool_registry, loop_config, &cfg, &task_manager,
                                       cwd,
                                       /*external_compactor=*/nullptr,
                                       /*event_bus=*/nullptr,
                                       /*touch_collector=*/nullptr,
                                       /*file_index_invalidator=*/nullptr, session_id);
}

/// @brief 同步执行一轮 ReAct 循环
/// @param streamed 输出参数：stream-json 模式下累积的逐步 NDJSON
ReActResult execute_task(ReActLoop& loop, const std::string& task,
                         const nlohmann::json& tools_schema, const std::string& sys_prompt,
                         const std::string& output_format, std::string& streamed) {
    std::vector<ChatMessage> messages;
    messages.push_back(ChatMessage::user(task));
    std::atomic<bool> should_cancel{false};

    // 步骤回调：stream-json 模式需要逐 step 输出
    ReActLoop::StepCallback on_step = nullptr;
    if (output_format == "stream-json") {
        on_step = [&streamed](const ReActStep& step) { streamed += step_json(step).dump() + "\n"; };
    }
    return loop.run(messages, sys_prompt, tools_schema, should_cancel, std::move(on_step),
                    /*on_token=*/nullptr);
}

/// @brief 已确定后端后的完整执行流程（权限 / 工具 / 循环 / 输出 / 退出码）
/// @details run_headless 与 run_headless_with_provider 共用此实现，保证
///          「解析后端」与「注入后端」两条路径除后端来源外行为完全一致。
HeadlessResult run_with_provider(IConfigManager& cfg, ITaskManager& task_manager,
                                 const HeadlessOptions& opts, ICompletionProvider* provider) {
    HeadlessResult result;

    // ---- 1. 权限模式（headless 无人值守档）----
    auto pm = parse_permission_mode(opts.permission_mode);
    if (!pm) {
        result.exit_code = 2;
        result.output = "error: 未知权限模式 '" + opts.permission_mode +
                        "'（可选 default / accept-edits / bypass-permissions）\n";
        return result;
    }

    // ---- 2. 注册内置工具 + 系统提示词 ----
    auto tool_registry = std::make_shared<tool::ToolRegistry>();
    // MCP：headless 下也尝试连接（与 create_session 一致；空 manager 则 MCP 工具返回"未连接"）
    std::shared_ptr<mcp::McpClientManager> mcp_manager;
    register_builtin_tools(*tool_registry, mcp_manager);

    const std::string user_prompt = cfg.get_or<std::string>(keys::SYSTEM_PROMPT, "");
    const std::string sys_prompt = build_system_prompt(user_prompt, *tool_registry);

    // ---- 2.5 #126：--goal 覆盖目标声明 ----
    // 必须在 build_loop()（内部调 apply_verification_gate 读 agent.goal）之前落盘，
    // 否则显式指定的目标会被忽略，门禁依旧按"无目标"静默放行。
    // 优先级：--goal > WORKX_GOAL > config.json —— 命令行在最后写入，天然最高。
    if (!opts.goal.empty()) {
        // 先解析再写入：拼错的目标（例如 "tests-pass"）会让 parse_goal 返回 None，
        // 门禁随后按"无目标"静默放行 —— 那正是 #126 要消灭的静默失效，必须当场报错。
        if (parse_goal(opts.goal).type == AgentGoal::None) {
            result.exit_code = 2;
            result.output = "error: 无法识别的 --goal '" + opts.goal +
                            "'（可选 tests_pass / build_clean / lint_zero / "
                            "file_exists:<path> / cmd:<command>）\n";
            return result;
        }
        // ⚠️ ResultV2<void> 没有 operator bool，只有 is_ok()/is_err()
        if (auto r = cfg.set(keys::AGENT_GOAL, opts.goal); r.is_err()) {
            result.exit_code = 2;
            result.output = "error: --goal 写入配置失败：" + r.error().message + "\n";
            return result;
        }
        LOG_INFO("[headless] #126 goal overridden by --goal: {}", opts.goal);
    }

    // ---- 3. 构造循环并执行 ----
    const std::string session_id = core::util::generate_uuid();
    auto loop = build_loop(cfg, task_manager, provider, tool_registry, session_id);
    loop->set_permission_mode(*pm);

    // ---- 4. 同步执行 + 输出 + 退出码 ----
    std::string streamed;
    auto react_result = execute_task(*loop, opts.task, tool_registry->get_all_schemas(), sys_prompt,
                                     opts.output_format, streamed);

    render_output(opts, react_result, session_id, std::move(streamed), result.output);
    result.exit_code = derive_exit_code(react_result);
    return result;
}

}  // namespace

/// @brief 按输出格式序列化 ReActResult
/// @param streamed stream-json 模式下已累积的逐步输出，需在其后追加最终结果
void render_output(const HeadlessOptions& opts, const ReActResult& react_result,
                   const std::string& session_id, std::string streamed, std::string& out) {
    if (opts.output_format == "json") {
        out = result_json(react_result, session_id).dump(2) + "\n";
    } else if (opts.output_format == "stream-json") {
        out = std::move(streamed) + result_json(react_result, session_id).dump() + "\n";
    } else {
        out = result_text(react_result);
        if (out.empty()) out = react_result.error_message;
        out += "\n";
        // #81：文本模式同样附改动清单（无人值守时无法事后追问"改了什么"）
        const std::string diff_text = git_diff_summary_text();
        if (!diff_text.empty()) {
            out += "\n--- 改动清单 ---\n" + diff_text;
        }
    }
}

HeadlessResult run_headless(IConfigManager& cfg, ITaskManager& task_manager, IEventBus& event_bus,
                            const HeadlessOptions& opts) {
    // ---- 1. 创建后端 ----
    auto backend_result = resolve_backend(cfg, event_bus);
    if (backend_result.remote_url.empty() || !backend_result.provider) {
        HeadlessResult result;
        result.exit_code = 2;  // 参数/配置错误
        result.output = "error: 无法创建后端（缺少 remote_url / provider 配置）\n";
        return result;
    }

    // ---- 2. 执行（与注入路径共用实现，保证两条路径行为一致）----
    return run_with_provider(cfg, task_manager, opts, backend_result.provider.get());
}

HeadlessResult run_headless_with_provider(IConfigManager& cfg, ITaskManager& task_manager,
                                          const HeadlessOptions& opts,
                                          ICompletionProvider* provider) {
    if (provider == nullptr) {
        HeadlessResult result;
        result.exit_code = 2;
        result.output = "error: 未提供后端（provider == nullptr）\n";
        return result;
    }
    return run_with_provider(cfg, task_manager, opts, provider);
}

}  // namespace agent
