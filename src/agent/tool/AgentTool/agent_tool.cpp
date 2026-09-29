/**
 * @file agent_tool.cpp
 * @brief AgentTool 实现
 * @details 子 Agent 调度工具的具体实现
 * @version 1.4.0
 * @date 2026-07
 */

#include "agent/tool/AgentTool/agent_tool.h"

#include <random>
#include <format>
#include <algorithm>
#include <mutex>

#include "agent/api/chat_types.h"
#include "agent/core/agent_task_id.h"
#include "agent/core/react_loop.h"
#include "agent/core/react_step_format.h"
#include "agent/hook/hook_event.h"  // #50 通用 Hook 事件系统：SubagentStart/SubagentStop
#include "agent/hook/hook_manager.h"
#include "agent/mcp/mcp_client_manager.h"  // #56 方案 D：子作用域 MCP 管理器
#include "agent/tool/MCPTool/mcp_tool.h"   // H-1：子 Agent 绑定作用域 manager 的 MCPTool
#include "agent/skill/inclaude/skill_prompt.h"  // #56 方案 C：build_skill_full_text 共享取全文
#include "core/task/task_manager.h"
#include "core/utils/error.h"
#include "core/events/agent_events.h"

namespace agent::tool {

namespace {

/// @brief 用 ", " 连接任务 id 列表（v1.2.0 批量调度返回消息）
std::string fmt_join_ids(const std::vector<std::string>& ids) {
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += ids[i];
    }
    return out;
}

/// @brief 格式化任务数量描述（单复数："1 task" / "3 tasks"）
std::string fmt_task_count(size_t n) { return std::format("{} {}", n, n == 1 ? "task" : "tasks"); }

}  // namespace

/// @brief 启动单个子 Agent 任务
/// @details v1.2.0：并行批量调度复用。为每个任务生成独立 task_id 并投递到线程池，
///          多个子 Agent 并发执行。返回已启动的 Task（供调用方等待/读输出）。
/// @param options 子 Agent 启动参数
/// @return 已启动的 Task
std::shared_ptr<agent::Task> launch_sub_agent(const SubAgentLaunchOptions& options) {
    return options.task_manager->launch(
        options.task_id, [options](const std::atomic<bool>& should_cancel) {
            // v1.1.0：为子 Agent 构建独立工具集（不共享父 registry 的暴露面）
            //  - 白名单过滤：tools 为空使用全部已注册工具，否则仅保留白名单内工具
            //  - Plan 只读：父处于 Plan（只读）时仅保留只读工具，杜绝写/执行能力，
            //    与 check_permissions 形成双重防线（权限逃逸纵深防御）
            //  - 防递归：无论如何排除 Agent 工具本身，子 Agent 不能再启动子 Agent，
            //    杜绝无限嵌套/循环（即使白名单显式包含 "Agent" 也会被忽略）
            auto sub_agent_registry = std::make_shared<ToolRegistry>();
            // #56 方案 D：先构建 MCP 作用域（引用复用父 client 不清理；inline 新建需 dispose），
            //             供下方 registry 构建时为 MCP 工具绑定作用域 manager。
            AgentTool::McpScopeBuildResult mcp_scope =
                AgentTool::build_mcp_scope(options.mcp_servers, options.parent_mcp_manager);
            const bool have_mcp_scope = mcp_scope.scope && !mcp_scope.scope->empty();
            if (options.sub_registry) {
                std::vector<std::shared_ptr<ITool>> candidates =
                    options.tool_whitelist.empty() ? options.sub_registry->get_all_tools() : [&] {
                        std::vector<std::shared_ptr<ITool>> sel;
                        for (const auto& name : options.tool_whitelist) {
                            if (auto t = options.sub_registry->find_by_name(name)) {
                                sel.push_back(t);
                            }
                        }
                        return sel;
                    }();
                for (const auto& t : candidates) {
                    if (t->name() == kAgentToolName) {
                        continue;  // 防递归：子 Agent 不携带 Agent 工具
                    }
                    if (options.permission_mode == tool::PermissionMode::Plan &&
                        !t->is_read_only()) {
                        continue;  // Plan 只读：跳过写/执行工具
                    }
                    // H-1：子 Agent 定义 mcpServers 时，为其 MCP 工具绑定作用域 manager，
                    //      使 prompt() 展示的 server 列表与 call() 实际可用范围一致；
                    //      否则复用的共享实例 prompt() 固定显示父全局 server 列表。
                    if (t->name() == "MCP" && have_mcp_scope) {
                        sub_agent_registry->register_tool(
                            std::make_shared<MCPTool>(mcp_scope.scope));
                        continue;
                    }
                    sub_agent_registry->register_tool(t);
                }
            }

            // 子会话：全新消息历史，system_prompt = 任务 prompt
            // #30：注入父会话 ID，使子 Agent 工具调用的审计日志可关联到同一会话
            ReActLoop loop(options.provider, sub_agent_registry, options.config_manager,
                           options.task_manager, options.cwd, options.event_bus,
                           options.session_id);
            // 评审 #1：子 Agent 继承父会话权限模式，避免 Plan 只读边界被绕过
            loop.set_permission_mode(options.permission_mode);
            if (have_mcp_scope) {
                loop.set_mcp_manager(mcp_scope.scope);
            }
            std::vector<ChatMessage> messages;
            // #56 方案 C：按 skill 名预加载全文到初始 system 消息（找不到/非技能静默跳过）
            auto preload = AgentTool::build_skill_preload_messages(
                options.skills, options.command_registry,
                command::CommandContext{.cwd = options.cwd, .session_id = options.session_id});
            if (!preload.empty()) {
                messages.insert(messages.end(), preload.begin(), preload.end());
            }
            // 独立工具集 schema（子 Agent 的 ToolContext.tool_registry 亦指向独立 registry）
            nlohmann::json tools_schema = sub_agent_registry->get_all_schemas();

            // #50 SubagentStart hook：子 Agent 构建完成、run() 之前派发（父作用域）
            if (options.hook_manager && !options.hook_manager->empty()) {
                hook::HookContext hctx;
                hctx.session_id = options.session_id;
                hctx.cwd = options.cwd;
                hctx.subagent_id = options.task_id;
                hctx.subagent_prompt = options.prompt;
                options.hook_manager->dispatch(hook::HookEvent::SubagentStart, hctx);
            }

            auto task_ptr = options.task_manager->find_task(options.task_id);
            ReActResult result = loop.run(
                messages, options.prompt, tools_schema, should_cancel,
                [options, &task_ptr](const ReActStep& step) {
                    const std::string line = format_step_line(step);
                    if (task_ptr && !line.empty()) {
                        task_ptr->append_output(line);
                    }
                    // v1.2.0 子任务进度流式订阅：每个步骤增量推送结构化进度事件，
                    // 使订阅者按 task_id 实时跟踪子任务进度（无需轮询 TaskOutput）。
                    // 仅作增量通知，不注入父 LLM 上下文。
                    // L-3：空文本步骤不发布进度事件，避免空 content 噪音
                    if (options.event_bus != nullptr && !line.empty()) {
                        options.event_bus->publish_async(SubAgentProgressEvent{
                            .task_id = options.task_id,
                            .step_number = step.step_number,
                            .step_type = step_type_str(step.type),
                            .content = line,
                            // v1.3.0 结构化字段：第二层卡片渲染复用主会话 UI
                            .thought_text = step.thought_text,
                            .tool_name = step.tool_name,
                            .tool_input =
                                step.tool_input.is_null() ? std::string{} : step.tool_input.dump(),
                            .observation = step.observation,
                            .is_error = step.is_error,
                            .duration_ms = step.duration_ms});
                    }
                });

            // 收尾：错误或最终答案写入输出缓冲
            std::string summary;
            if (result.was_error) {
                summary = std::format("Error: {}", result.error_message);
            } else if (!result.final_answer.empty()) {
                summary = std::format("Final: {}", result.final_answer);
            }
            if (task_ptr && !summary.empty()) {
                task_ptr->append_output(summary);
            }

            // #50 SubagentStop hook：子 Agent run() 返回、收尾后派发（父作用域）。
            // 与设计 4.3 对齐：子 ReActLoop 自身不重复触发 Stop，此处折叠为 SubagentStop。
            if (options.hook_manager && !options.hook_manager->empty()) {
                hook::HookContext hctx;
                hctx.session_id = options.session_id;
                hctx.cwd = options.cwd;
                hctx.subagent_id = options.task_id;
                hctx.subagent_prompt = options.prompt;
                hctx.final_answer = summary;
                hctx.stop_reason = result.was_error ? "error" : "completed";
                options.hook_manager->dispatch(hook::HookEvent::SubagentStop, hctx);
            }

            // v1.1.0 后台结果自动回送：子 Agent 完成后发布完成事件，
            // 使父会话/UI 等订阅者无需轮询 TaskOutput 即可感知结果。
            // 仅携带 task_id + 结果摘要，不注入父 LLM 上下文（避免刷屏父会话）。
            // 无条件发布（即使摘要为空），保证订阅者总能感知任务收尾（评审 #49 L-5）。
            if (options.event_bus != nullptr) {
                options.event_bus->publish_async(SubAgentCompletedEvent{
                    .task_id = options.task_id,
                    .final_answer = summary,
                    .was_error = result.was_error,
                    .duration_ms = static_cast<double>(result.total_duration_ms)});
            }

            // #56 方案 D：子 Agent 收尾清理 inline 私有 MCP client（引用复用 client 不在此列）
            if (mcp_scope.scope) {
                for (auto& c : mcp_scope.owned_clients) {
                    mcp_scope.scope->dispose(c);
                }
            }
        });
}

const std::string& AgentTool::name() const {
    static const std::string n{kAgentToolName};
    return n;
}

const std::string& AgentTool::description() const {
    static const std::string d{"Launches a sub-agent to handle a complex task."};
    return d;
}

const std::string& AgentTool::prompt() const {
    static const std::string p{
        "Launches a sub-agent with a specific prompt and tool set. "
        "The sub-agent runs independently and returns its result."};
    return p;
}

nlohmann::json AgentTool::input_schema() const {
    // v1.2.0：新增 tasks 数组支持并行批量调度；
    // 提供 tasks 时按数组逐项并行启动子 Agent，否则回退到单个 prompt
    // v1.3.x：新增 skills（#56 方案 C）与 mcpServers（#56 方案 D）。
    // 注意：mcpServers 的 items 内嵌 oneOf 两个并排对象在深嵌套 brace-init-list 下
    // 会触发 MSVC 的 brace-ambiguation（C2059/C2143），故先显式构造该子 schema，
    // 再以 json 值引用，避免歧义。
    nlohmann::json mcp_server_schema = nlohmann::json::object();
    mcp_server_schema["type"] = "array";
    nlohmann::json one_of = nlohmann::json::array();
    one_of.push_back({{"type", "string"}});
    one_of.push_back({{"type", "object"}});
    nlohmann::json items = nlohmann::json::object();
    items["oneOf"] = one_of;
    mcp_server_schema["items"] = items;
    mcp_server_schema["description"] =
        "#56: MCP servers for the sub-agent (string=reuse global client; "
        "object=connect fresh client, closed when sub-agent ends)";

    const nlohmann::json skills_schema = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description",
         "#56: Skills whose full text is preloaded into the sub-agent's "
         "initial system messages"}};

    return {
        {"type", "object"},
        {"properties",
         {{"prompt",
           {{"type", "string"},
            {"description", "Single task prompt (used when 'tasks' is omitted)"}}},
          {"tasks",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"prompt", {{"type", "string"}, {"description", "Task prompt for one sub-agent"}}},
                {"tools",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}},
                  {"description",
                   "Allowed tools for this task (whitelist; empty uses all registered tools)"}}},
                {"skills", skills_schema},
                {"mcpServers", mcp_server_schema}}},
              {"required", {"prompt"}},
              {"additionalProperties", false}}},
            {"description",
             "Batch of sub-agent tasks to launch in parallel (each gets its own task_id)"}}},
          {"tools",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "Allowed tools for the single sub-agent (whitelist; empty/omitted uses all registered "
             "tools)"}}},
          {"skills", skills_schema},
          {"mcpServers", mcp_server_schema},
          {"run_in_background",
           {{"type", "boolean"},
            {"description",
             "Run the sub-agent(s) in background (default true); false waits for completion"}}}}},
        {"anyOf", nlohmann::json::array({{{"required", nlohmann::json::array({"prompt"})}},
                                         {{"required", nlohmann::json::array({"tasks"})}}})},
        {"additionalProperties", false}};
}

ResultV2<ToolResult> AgentTool::call(const nlohmann::json& input, const ToolContext& ctx) const {
    // 1. 校验输入
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "Agent: input must be an object");
    }
    const bool run_in_background = input.value("run_in_background", true);

    // v1.1.0：解析顶级 tools 白名单（空/缺失 → 使用全部已注册工具；仅单任务模式生效）
    std::vector<std::string> tool_whitelist;
    if (input.contains("tools") && input["tools"].is_array()) {
        for (const auto& item : input["tools"]) {
            if (item.is_string()) {
                tool_whitelist.push_back(item.get<std::string>());
            }
        }
    }

    // v1.2.0：解析批量任务（tasks 数组 → 并行）或单个 prompt（向后兼容）
    struct SubTaskSpec {
        std::string prompt;
        std::vector<std::string> tools;
        std::vector<std::string> skills;  // #56 方案 C：子 Agent 预加载 skill 名
        nlohmann::json mcp_servers;  // #56 方案 D：mcpServers（字符串引用 / inline 对象数组）
    };

    // #56 方案 C：解析 skills（string[]，单任务 & 批量任务共用）
    auto parse_skills = [](const nlohmann::json& j) -> std::vector<std::string> {
        std::vector<std::string> out;
        if (!j.is_array()) return out;
        for (const auto& s : j) {
            if (s.is_string()) out.push_back(s.get<std::string>());
        }
        return out;
    };

    std::vector<SubTaskSpec> specs;
    if (input.contains("tasks") && input["tasks"].is_array() && !input["tasks"].empty()) {
        for (const auto& item : input["tasks"]) {
            if (!item.is_object()) continue;
            const std::string p = item.value("prompt", "");
            if (p.empty()) continue;
            SubTaskSpec spec;
            spec.prompt = p;
            if (item.contains("tools") && item["tools"].is_array()) {
                for (const auto& t : item["tools"]) {
                    if (t.is_string()) spec.tools.push_back(t.get<std::string>());
                }
            }
            spec.skills = parse_skills(item.value("skills", nlohmann::json::array()));
            // #56 方案 D：逐任务 mcpServers（字符串引用 / inline 对象数组）
            spec.mcp_servers = item.value("mcpServers", nlohmann::json::array());
            specs.push_back(std::move(spec));
        }
    }
    if (specs.empty()) {
        // 单任务模式（向后兼容）
        const std::string prompt = input.value("prompt", "");
        if (prompt.empty()) {
            return ResultV2<ToolResult>::err(
                Error::Code::MissingArgument,
                "Agent: 'prompt' is required (or provide a non-empty 'tasks' array)");
        }
        SubTaskSpec spec;
        spec.prompt = prompt;
        spec.tools = tool_whitelist;
        spec.skills = parse_skills(input.value("skills", nlohmann::json::array()));
        // #56 方案 D：单任务顶级 mcpServers（字符串引用 / inline 对象数组）
        spec.mcp_servers = input.value("mcpServers", nlohmann::json::array());
        specs.push_back(std::move(spec));
    }

    if (ctx.provider_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::NotImplemented,
                                         "Agent: no LLM provider available");
    }
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::NotImplemented,
                                         "Agent: no task manager available");
    }
    if (ctx.config_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(Error::Code::NotImplemented,
                                         "Agent: no config manager available");
    }

    // 捕获子 Agent 上下文（指针均由调用方保证存活于会话周期）
    ICompletionProvider* provider = ctx.provider_ptr;
    std::shared_ptr<ToolRegistry> sub_registry = ctx.tool_registry;
    IConfigManager* config_manager = ctx.config_manager_ptr;
    ITaskManager* task_manager = ctx.task_manager_ptr;
    IEventBus* event_bus = ctx.event_bus_ptr;
    std::string cwd = ctx.cwd;
    // #26 评审 #1：继承父会话权限模式，防止子 Agent 提升权限（父 Plan 只读时子 Agent 也受限）
    const tool::PermissionMode permission_mode = ctx.permission_mode;
    // #50：父循环 HookManager（子 Agent 作用域 SubagentStart/Stop 派发）
    std::shared_ptr<agent::hook::HookManager> hook_manager = ctx.hook_manager_ptr;

    // 2. 并行启动所有子 Agent（各自独立 task_id，线程池并发执行）
    std::vector<std::shared_ptr<agent::Task>> tasks;
    std::vector<std::string> ids;
    tasks.reserve(specs.size());
    ids.reserve(specs.size());
    for (const auto& spec : specs) {
        const std::string task_id = generate_agent_task_id('a');
        ids.push_back(task_id);
        tasks.push_back(launch_sub_agent(SubAgentLaunchOptions{
            .task_id = task_id,
            .prompt = spec.prompt,
            .tool_whitelist = spec.tools,
            .skills = spec.skills,  // #56 方案 C：子 Agent 预加载 skill 名
            .command_registry = ctx.command_registry_ptr,  // #56 方案 C：按名取 skill 全文
            .provider = provider,
            .sub_registry = sub_registry,
            .config_manager = config_manager,
            .task_manager = task_manager,
            .event_bus = event_bus,
            .cwd = cwd,
            .session_id = ctx.session_id,  // #30：父会话 ID 传递给子 Agent
            .permission_mode = permission_mode,
            .hook_manager = hook_manager,     // #50：父作用域 HookManager
            .mcp_servers = spec.mcp_servers,  // #56 方案 D：子 Agent MCP 作用域配置
            .parent_mcp_manager =
                ctx.mcp_manager_ptr,  // #56 方案 D：父全局 manager（引用复用来源）
        }));
    }

    // 3. 同步模式：并行等待全部任务完成再返回（M-2：waitForTasks 单一 30s 兜底，
    //    替代逐个 wait 的最坏 N×30s）。超时后返回但任务可能仍在运行，返回时明确提示，
    //    避免工具调用线程无限阻塞（评审 #3）
    if (!run_in_background) {
        task_manager->waitForTasks(tasks);
        const bool all_finished =
            std::all_of(tasks.begin(), tasks.end(), [](const auto& t) { return t->isFinished(); });
        if (!all_finished) {
            std::string partial;
            for (size_t i = 0; i < tasks.size(); ++i) {
                partial += std::format("--- {} ---\n{}\n", ids[i], tasks[i]->output());
            }
            return ResultV2<ToolResult>::ok(ToolResult::ok(
                std::format("Sub-agent(s) timed out waiting ({}): {}. They may still be running; "
                            "use TaskStop to cancel. Partial output:\n{}",
                            fmt_task_count(ids.size()), fmt_join_ids(ids), partial)));
        }
        std::string out;
        for (size_t i = 0; i < tasks.size(); ++i) {
            out += std::format("--- {} ---\n{}\n", ids[i], tasks[i]->output());
        }
        return ResultV2<ToolResult>::ok(ToolResult::ok(
            std::format("Sub-agents completed ({}).\n{}", fmt_task_count(ids.size()), out)));
    }

    // 后台模式：立即返回全部 task_id
    if (ids.size() == 1) {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
            "Sub-agent launched (task: {}). Use TaskOutput to read its progress.", ids[0])));
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(
        std::format("Sub-agents launched ({}): {}. Use TaskOutput to read their progress.",
                    fmt_task_count(ids.size()), fmt_join_ids(ids))));
}

std::vector<agent::ChatMessage> AgentTool::build_skill_preload_messages(
    const std::vector<std::string>& skills, const command::CommandRegistry* registry,
    const command::CommandContext& cctx) {
    std::vector<agent::ChatMessage> out;
    if (!registry || skills.empty()) return out;
    for (const auto& name : skills) {
        if (name.empty()) continue;
        auto base = registry->find_by_name(name);
        // 仅预加载真正可注入全文的技能条目（prompt 类型 + Skills/Bundled 来源）；
        // #56 M-3：bundled 技能（loop/debug 等）同样注册进 CommandRegistry，应可预加载。
        // 其余（本地命令、内置 prompt、未知名）静默跳过，不阻断子 Agent 启动。
        if (!base || base->type() != "prompt" ||
            (base->loaded_from() != command::LoadSource::Skills &&
             base->loaded_from() != command::LoadSource::Bundled)) {
            continue;
        }
        auto cmd = std::dynamic_pointer_cast<const command::PromptCommand>(base);
        if (!cmd) continue;
        const std::string full = agent::skill::build_skill_full_text(*cmd, cctx);
        if (full.empty()) continue;
        // 每条 skill 生成一条 system 消息，首行标注技能名，正文为该技能全文
        out.push_back(agent::ChatMessage::system("Skill: " + name + "\n\n" + full));
    }
    return out;
}

AgentTool::McpScopeBuildResult AgentTool::build_mcp_scope(const nlohmann::json& servers,
                                                          mcp::McpClientManager* parent) {
    AgentTool::McpScopeBuildResult r;
    // 空 / 非数组 / 空数组 → 空作用域（调用方以 empty() 判定可用性）
    if (!servers.is_array() || servers.empty()) return r;

    auto scope = std::make_shared<mcp::McpClientManager>(nullptr);
    for (const auto& item : servers) {
        if (item.is_string()) {
            // 字符串引用：从父全局管理器复用已 memoized client，不 cleanup
            const std::string name = item.get<std::string>();
            if (name.empty() || !parent) continue;
            if (auto client = parent->get_client(name)) {
                scope->register_client(name, client);
            }
            // 父管理器无此 server → 静默跳过（不阻断子 Agent）
        } else if (item.is_object()) {
            // inline 对象：运行时新建独立连接，子 Agent 结束需 dispose
            mcp::McpServerConfig cfg;
            cfg.name = item.value("name", std::string{});
            cfg.command = item.value("command", std::string{});
            if (item.contains("args") && item["args"].is_array()) {
                for (const auto& a : item["args"]) {
                    if (a.is_string()) cfg.args.push_back(a.get<std::string>());
                }
            }
            if (item.contains("env") && item["env"].is_object()) {
                for (const auto& [k_, v] : item["env"].items()) {
                    if (v.is_string()) cfg.env[k_] = v.get<std::string>();
                }
            }
            cfg.url = item.value("url", std::string{});
            cfg.allow_private = item.value("allowPrivate", false);
            if (cfg.name.empty() || !cfg.valid()) continue;
            // 连接失败（如命令不存在）静默跳过，不抛异常（异常安全）
            if (auto client = scope->connect_one_off(cfg)) {
                scope->register_client(cfg.name, client);
                r.owned_clients.push_back(client);
            }
        }
    }
    r.scope = std::move(scope);
    return r;
}

}  // namespace agent::tool
