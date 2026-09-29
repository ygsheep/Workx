/**
 * @file hook_manager.cpp
 * @brief HookManager — 调度核心实现
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/hook/hook_manager.h"

#include <atomic>
#include <chrono>

#include "agent/api/i_completion_provider.h"
#include "agent/api/remote/http_client.h"
#include "agent/config/app_config.h"        // keys::HOOKS_ENABLED / HOOKS_DEFINITIONS
#include "agent/core/goal_guarded_agent.h"  // GoalAgentDeps / ReActLoopFactory（agent 类型子循环）
#include "agent/tool/registry.h"  // ToolRegistry（agent 类型白名单派生子工具集）
#include "core/config/i_config_manager.h"  // get_or<std::string>
#include "core/events/agent_events.h"      // HookProgressEvent（M-2 进度可视化）
#include "core/events/i_event_bus.h"       // IEventBus::publish_async
#include "core/process/subprocess.h"
#include "liblogger/logger.h"

namespace agent::hook {

// ============================================================
// 装配（QueryEngine / ChatSession 共用）
// ============================================================

std::shared_ptr<HookManager> make_hook_manager(
    agent::IConfigManager& cfg, std::shared_ptr<agent::tool::ToolRegistry> tool_registry,
    agent::ICompletionProvider* provider, agent::IEventBus* bus) {
    auto manager = std::make_shared<HookManager>();
    manager->set_provider(provider);    // prompt/agent 类型需要（非拥有）
    manager->set_event_bus(bus);        // hook 进度事件（可选）
    manager->set_config_manager(&cfg);  // agent 类型子 ReActLoop 需要
    manager->set_tool_registry(std::move(tool_registry));  // agent 白名单来源

    const bool enabled = cfg.get_or<bool>(agent::keys::HOOKS_ENABLED, true);
    if (!enabled) return manager;  // 空 manager，调用方 empty() 短路

    const std::string defs_json = cfg.get_or<std::string>(agent::keys::HOOKS_DEFINITIONS, "[]");
    try {
        auto arr = nlohmann::json::parse(defs_json);
        if (arr.is_array()) {
            for (const auto& obj : arr) {
                hook::HookDefinition def = hook::HookDefinition::from_json(obj);
                if (def.command.empty() && def.url.empty() && def.prompt.empty()) {
                    LOG_WARN("[hook] hook def missing command/url/prompt, skipped: {}", obj.dump());
                    continue;
                }
                manager->register_hook(std::move(def));
            }
        }
    } catch (const std::exception& e) {
        LOG_WARN("[hook] invalid hooks.definitions JSON, hooks disabled: {}", e.what());
    }
    return manager;
}

namespace {
constexpr size_t MAX_HOOK_OUTPUT_BYTES = 64 * 1024;  ///< 单条 hook 输出上限 64KB
constexpr int kAgentMaxIterations = 4;  ///< agentic verifier 子 ReActLoop 最大轮数

/// @brief 单调递增 hook 执行 id（供 HookProgressEvent start/done 关联；线程安全）
uint64_t next_hook_id() {
    static std::atomic<uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

/// @brief 生成 hook 展示标签（UI 卡片标题；command 显示命令，http 显示 URL，
///        prompt/agent 显示截断提示词）。仅供展示，不参与语义。
std::string hook_label(const HookDefinition& def) {
    if (!def.command.empty()) return "[c] " + def.command;
    if (!def.url.empty()) return "[http] " + def.url;
    if (!def.prompt.empty()) {
        std::string p = def.prompt;
        if (p.size() > 60) p = p.substr(0, 60) + "…";
        return "[prompt] " + p;
    }
    return "[" + std::string(type_to_string(def.type)) + "]";
}

std::string truncate_output(const std::string& text, size_t max_chars) {
    if (text.size() <= max_chars) return text;
    return text.substr(0, max_chars) + "\n...(truncated)";
}

/// @brief 解析 hook 命令输出中的阻断指令 JSON，如 `{"blockingError":"..."}` 或
///        `{"preventContinuation":true}`（对齐 cc）。未命中则返回默认结果。
void apply_command_output(HookResult& r, const std::string& output) {
    const std::string trimmed = [&] {
        const auto b = output.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string{};
        return output.substr(b);
    }();
    if (trimmed.empty() || trimmed[0] != '{') return;  // 非 JSON（普通输出）
    try {
        auto obj = nlohmann::json::parse(trimmed);
        if (obj.contains("message") && obj["message"].is_string())
            r.message = obj["message"].get<std::string>();
        if (obj.contains("blockingError")) {
            if (obj["blockingError"].is_string())
                r.blockingError = obj["blockingError"].get<std::string>();
            else if (obj["blockingError"].is_boolean() && obj["blockingError"].get<bool>())
                r.blockingError = r.message.empty() ? std::string("blocked") : r.message;
        }
        if (obj.contains("preventContinuation") && obj["preventContinuation"].is_boolean())
            r.preventContinuation = obj["preventContinuation"].get<bool>();
    } catch (const std::exception&) {
        // 普通文本，保留原样
    }
}
}  // anonymous namespace

// ============================================================
// 注册
// ============================================================

void HookManager::register_hook(HookDefinition def) {
    // 同 event+type+match 去重（config 与 frontmatter 可能重复注册）
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : entries_) {
        if (e.def.event == def.event && e.def.type == def.type && e.def.match == def.match &&
            e.def.command == def.command && e.def.url == def.url) {
            return;
        }
    }
    entries_.emplace_back(std::move(def));
}

void HookManager::register_hooks(std::vector<HookDefinition> defs) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    for (auto& d : defs) entries_.emplace_back(std::move(d));
}

void HookManager::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
}

// ============================================================
// dispatch
// ============================================================

HookResult HookManager::dispatch(HookEvent event, const HookContext& ctx) {
    // 线程安全：锁内对匹配条目做快照 + 乐观占用 once，锁外顺序执行 run_hook，
    // 避免长耗时 hook（LLM/子进程）串行阻塞其他触发线程，同时保证并发安全性。
    HookResult out;
    std::vector<HookEntry*> selected;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.empty()) return out;

        const std::string event_name = to_string(event);
        for (auto& entry : entries_) {
            if (entry.def.event != event) continue;
            // once 语义：已执行过一次则跳过
            if (entry.def.once && entry.consumed) continue;

            if (!entry.matcher.matches(event_name, ctx.tool_name, ctx.tool_input)) {
                continue;
            }

            // async 类型：本版本不提供异步执行队列 → 退化为同步执行（记录日志）
            if (entry.def.async) {
                LOG_WARN("[hook] event={} async hook degrades to sync (no async queue)",
                         event_name);
            }

            if (entry.def.statusMessage) {
                LOG_INFO("[hook] running event={} type={} cmd={} url={}", event_name,
                         type_to_string(entry.def.type), entry.def.command, entry.def.url);
            }

            selected.push_back(&entry);
            if (entry.def.once) entry.consumed = true;  // 乐观占用：防并发重复执行
        }
    }

    for (HookEntry* entry : selected) {
        HookResult r = run_hook(*entry, ctx);
        aggregate(r, out);
        std::lock_guard<std::mutex> lock(mutex_);
        entry->run_count++;
    }
    return out;
}

// ============================================================
// 执行
// ============================================================

HookResult HookManager::run_hook(const HookEntry& entry, const HookContext& ctx) {
    // M-2：执行前发布 hook 开始进度事件（bus 为空则跳过，且不分配 hook_id）
    const uint64_t hook_id = event_bus_ ? next_hook_id() : 0;
    if (event_bus_) {
        agent::HookProgressEvent ev;
        ev.session_id = ctx.session_id;
        ev.hook_id = hook_id;
        ev.event = to_string(entry.def.event);
        ev.phase = "start";
        ev.hook_type = type_to_string(entry.def.type);
        ev.tool_name = ctx.tool_name;
        ev.hook_label = hook_label(entry.def);
        event_bus_->publish_async(ev);
    }

    HookResult r;
    switch (entry.def.type) {
        case HookType::Command:
            r = run_command(entry.def, ctx);
            break;
        case HookType::Http:
            r = run_http(entry.def, ctx);
            break;
        case HookType::Prompt:
            r = run_prompt(entry.def, ctx);
            break;
        case HookType::Agent:
            r = run_agent(entry.def, ctx);
            break;
    }

    // M-2：执行后发布完成/失败进度事件（复用同一 hook_id 供 UI 关联同一条执行）
    if (event_bus_) {
        agent::HookProgressEvent ev;
        ev.session_id = ctx.session_id;
        ev.hook_id = hook_id;
        ev.event = to_string(entry.def.event);
        ev.hook_type = type_to_string(entry.def.type);
        ev.tool_name = ctx.tool_name;
        ev.message = r.message;
        ev.hook_label = hook_label(entry.def);
        ev.phase = (r.blockingError && !r.blockingError->empty()) ? "failed" : "done";
        event_bus_->publish_async(ev);
    }
    return r;
}

void HookManager::aggregate(const HookResult& r, HookResult& out) const noexcept {
    if (r.blockingError && !out.blockingError) out.blockingError = r.blockingError;  // 首个阻断错误
    out.preventContinuation = out.preventContinuation || r.preventContinuation;
    if (!out.message.empty() && !r.message.empty()) out.message += "\n";
    out.message += r.message;
    if (!r.stopReason.empty()) out.stopReason = r.stopReason;
    if (!r.output.empty()) out.output += r.output;  // 注入上下文用
}

// ------------------------------------------------------------
// command 类型
// ------------------------------------------------------------

HookResult HookManager::run_command(const HookDefinition& def, const HookContext& ctx) {
    HookResult r;
    if (def.command.empty()) {
        r.message = "[hook] empty command, skipped";
        return r;
    }
    agent::process::ExecOptions opts;
    opts.cwd = ctx.cwd.empty() ? std::string(".") : ctx.cwd;
    opts.timeout = std::chrono::milliseconds(def.timeout_ms);
    opts.max_output_bytes = MAX_HOOK_OUTPUT_BYTES;
#if defined(_WIN32)
    const std::string command_line = "cmd.exe";
    opts.args = {"/d", "/s", "/c", def.command};
#else
    const std::string command_line = "sh";
    opts.args = {"-c", def.command};
#endif
    auto res = agent::process::exec(command_line, opts);
    if (res.is_ok() && res.value().exit_code == 0) {
        const auto& out = res.value();
        std::string combined = out.stdout_text;
        if (!out.stderr_text.empty()) {
            if (!combined.empty()) combined += "\n";
            combined += out.stderr_text;
        }
        r.output = truncate_output(combined, 4096);
        r.message = "[hook:command] ok\n" + r.output;
    } else if (res.is_ok()) {
        const auto& out = res.value();
        std::string combined = out.stdout_text;
        if (!out.stderr_text.empty()) {
            if (!combined.empty()) combined += "\n";
            combined += out.stderr_text;
        }
        r.message = "[hook:command] fail (exit " + std::to_string(out.exit_code) + ")\n" +
                    truncate_output(combined, 4096);
        r.output = r.message;
    } else {
        r.message = "[hook:command] error: " + res.error().message;
        r.output = r.message;
    }
    return r;
}

// ------------------------------------------------------------
// http 类型
// ------------------------------------------------------------

HookResult HookManager::run_http(const HookDefinition& def, const HookContext& ctx) {
    HookResult r;
    if (def.url.empty()) {
        r.message = "[hook:http] empty url, skipped";
        return r;
    }
    // 复用现有 HttpClient（SSRF 防护默认关闭，内部 hook 端点可能在内网）
    agent::HttpClient client;
    std::vector<std::pair<std::string, std::string>> headers;
    if (def.headers.is_object()) {
        for (auto it = def.headers.begin(); it != def.headers.end(); ++it) {
            if (it.value().is_string()) {
                headers.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
    }
    // POST 原始体（允许 hook 通过 command/body 传入，简单起见发 match 上下文 JSON）
    nlohmann::json payload = nlohmann::json::object();
    payload["event"] = to_string(def.event);
    payload["tool_name"] = ctx.tool_name;
    payload["session_id"] = ctx.session_id;
    payload["cwd"] = ctx.cwd;

    auto res = client.post_json(def.url, headers, payload, def.timeout_ms);
    if (res.is_ok()) {
        const auto& rep = res.value();
        r.output = truncate_output(rep.body, 4096);
        if (rep.is_success()) {
            r.message =
                "[hook:http] ok (HTTP " + std::to_string(rep.status_code) + ")\n" + r.output;
        } else {
            r.message =
                "[hook:http] fail (HTTP " + std::to_string(rep.status_code) + ")\n" + r.output;
        }
    } else {
        r.message = "[hook:http] error: " + res.error().message;
        r.output = r.message;
    }
    return r;
}

// ------------------------------------------------------------
// prompt / agent 类型（依赖 LLM provider，需事件上下文注入）
// ------------------------------------------------------------

HookResult HookManager::run_prompt(const HookDefinition& def, const HookContext& ctx) {
    HookResult r;
    if (!provider_) {
        r.message = "[hook:prompt] not ready (no provider wired)";
        r.output = r.message;
        return r;
    }
    if (def.prompt.empty()) {
        r.message = "[hook:prompt] empty prompt, skipped";
        return r;
    }

    // 事件/工具上下文注入到提示中，评估是否允许继续
    std::string prompt = def.prompt;
    {
        std::string ctx_block = "\n\n[hook context]\n";
        ctx_block += "event: " + std::string(to_string(def.event)) + "\n";
        if (!ctx.tool_name.empty()) ctx_block += "tool: " + ctx.tool_name + "\n";
        if (!ctx.tool_input.is_null() && !ctx.tool_input.is_object()) {
            ctx_block += "input: " + ctx.tool_input.dump() + "\n";
        } else if (ctx.tool_input.is_object()) {
            for (auto it = ctx.tool_input.begin(); it != ctx.tool_input.end(); ++it) {
                if (it.value().is_string())
                    ctx_block += it.key() + ": " + it.value().get<std::string>() + "\n";
            }
        }
        prompt += ctx_block;
    }

    CompletionRequest req;
    req.stream = false;
    req.temperature = 0.0f;
    req.max_tokens = 200;
    req.messages.push_back(ChatMessage::system(
        "你是 Agent Hook 策略评估器。根据下面的提示与事件上下文，判断是否放行/阻断，"
        "只用一行 JSON 回答：{\"blockingError\":\"理由\" 或 "
        "\"\",\"preventContinuation\":true/false,\"message\":\"附加提示(可空)\"}。"
        "默认放行为准，除非确有理由阻断。"));
    req.messages.push_back(ChatMessage::user(prompt));

    auto reader = provider_->submit_completion(req);
    if (!reader) {
        r.message = "[hook:prompt] provider submit failed, default allow";
        r.output = r.message;
        return r;
    }

    std::string text;
    StreamChunk chunk;
    while (true) {
        StreamState st = reader->next([]() { return false; }, chunk);
        if (st == StreamState::HasData || st == StreamState::Complete) {
            text += chunk.content_delta;
            if (st == StreamState::Complete) break;
        } else {
            break;  // Error / Cancelled
        }
    }

    r.output = truncate_output(text, 4096);
    r.message = "[hook:prompt] " + (text.empty() ? std::string("empty reply (allow)") : r.output);
    apply_command_output(r, text);  // 复用 JSON 判定解析
    return r;
}

std::shared_ptr<agent::tool::ToolRegistry> HookManager::get_sub_registry() {
    std::lock_guard<std::mutex> lock(tool_mutex_);
    if (cached_sub_registry_) return cached_sub_registry_;
    auto sub = std::make_shared<tool::ToolRegistry>();
    if (tool_registry_) {
        for (const auto& t : tool_registry_->get_all_tools()) {
            if (t->is_read_only()) sub->register_tool(t);
        }
    }
    cached_sub_registry_ = sub;
    return cached_sub_registry_;
}

HookResult HookManager::run_agent(const HookDefinition& def, const HookContext& ctx) {
    HookResult r;
    if (!provider_) {
        r.message = "[hook:agent] not ready (no provider wired)";
        r.output = r.message;
        return r;
    }
    if (!config_manager_) {
        r.message = "[hook:agent] not ready (no config_manager wired)";
        r.output = r.message;
        return r;
    }
    if (def.prompt.empty()) {
        r.message = "[hook:agent] empty prompt, skipped";
        return r;
    }

    // 受限子工具集：从来源 registry 懒构建并缓存（M-1 复用，来源稳定时只建一次）。
    // 只读过滤已排除 Agent/AskUser 等写/交互工具，杜绝 agent hook 递归启动子 Agent
    // 或向用户提问。来源未注入时退化为空子工具集（纯 prompt 评估仍可给出最终判定）。
    auto sub_registry = get_sub_registry();

    // 事件/工具上下文注入系统提示，供 verifier 核实
    std::string system_prompt =
        "你是 Agent Hook 策略评估器。你可以调用白名单只读工具核实事实，再根据下面"
        "的提示与事件上下文判断是否放行/阻断。最后必须只用一行 JSON 回答："
        "{\"blockingError\":\"理由\" 或 \"\",\"preventContinuation\":true/false,"
        "\"message\":\"附加提示(可空)\"}。默认放行为准，除非确有理由阻断。\n\n";
    system_prompt += def.prompt;
    system_prompt += "\n\n[hook context]\n";
    system_prompt += "event: " + std::string(to_string(def.event)) + "\n";
    if (!ctx.tool_name.empty()) system_prompt += "tool: " + ctx.tool_name + "\n";
    if (ctx.tool_input.is_object()) {
        for (auto it = ctx.tool_input.begin(); it != ctx.tool_input.end(); ++it) {
            if (it.value().is_string())
                system_prompt += it.key() + ": " + it.value().get<std::string>() + "\n";
        }
    }

    GoalAgentDeps deps;
    deps.provider = provider_;
    deps.registry = sub_registry;
    deps.config_manager = config_manager_;
    deps.cwd = ctx.cwd;
    deps.event_bus = event_bus_;
    deps.session_id = ctx.session_id;

    ReActLoop::Config loop_cfg;
    loop_cfg.max_iterations = kAgentMaxIterations;  // verifier 最大化受控，避免长跑
    auto loop = ReActLoopFactory::make(deps, sub_registry, loop_cfg);
    if (!loop) {
        r.message = "[hook:agent] failed to build sub loop, default allow";
        r.output = r.message;
        return r;
    }

    std::vector<ChatMessage> messages;
    nlohmann::json tools_schema = sub_registry->get_all_schemas();
    std::atomic<bool> cancel{false};  // agent hook 为同步阻塞执行，无外部取消通道
    auto result = loop->run(messages, system_prompt, tools_schema, cancel);

    const std::string final_text = result.final_answer;
    r.output = truncate_output(final_text, 4096);
    r.message =
        "[hook:agent] " + (final_text.empty() ? std::string("empty reply (allow)") : r.output);
    apply_command_output(r, final_text);  // 复用 JSON 判定解析（blockingError 等）
    return r;
}

}  // namespace agent::hook
