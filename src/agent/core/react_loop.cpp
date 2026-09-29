/**
 * @file react_loop.cpp
 * @brief ReActLoop 实现
 * @details Thought / Action / Observation 三阶段循环逻辑
 * @version 1.1.1
 * @date 2026-07
 */

#include "agent/core/react_loop.h"
#include "agent/core/react_observer.h"
#include "agent/api/i_stream_reader.h"
#include "agent/compact/prefix_shape.h"  // DS_CACHE M-2: normalize_tools_schema
#include "agent/config/app_config.h"     // #30：agent::keys::MODEL_NAME
#include "agent/hook/hook_manager.h"     // Issue #50：通用 Hook 事件系统
#include "agent/skill/inclaude/conditional.h"
#include "core/process/subprocess.h"       // #30：git 环境探测
#include "core/utils/uuid.h"               // #30：每次 turn 生成 request_id
#include "core/config/i_config_manager.h"  // #30：读取 backend.model_name
#include "liblogger/logger.h"

#include <cctype>
#include <cassert>
#include <deque>
#include <stdexcept>
#include <filesystem>
#include <format>
#include <future>
#include <thread>
#include <utility>
#include <algorithm>  // #30：std::reverse（历史摘要）
#include <vector>

namespace agent {

// ============================================================
// #30：环境感知辅助（git 探测 + 历史摘要）
// ============================================================

namespace {

/// @brief 执行 git 命令，返回去尾空白的 stdout；失败/非零退出码返回空串
/// @details 限时但不设取消回调（turnt 级探测，1500ms 内完成；git 命令通常 <100ms）
std::string git_stdout(const std::string& cwd, const std::vector<std::string>& args) {
    try {
        auto res = process::exec("git", process::ExecOptions{
                                            .cwd = cwd,
                                            .args = args,
                                            .timeout = std::chrono::milliseconds(1500),
                                        });
        if (res.is_ok() && res.value().exit_code == 0) {
            std::string out = res.value().stdout_text;
            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
                out.pop_back();
            }
            return out;
        }
    } catch (...) {
        // git 缺失/异常时不阻断 Agent，字段保持默认值
    }
    return {};
}

/// @brief #30：探测 git 环境并填充到 ToolContext
/// @details 非仓库时保持不变（git_branch/git_repo_root 空、git_has_uncommitted=false）
void detect_git_env(const std::string& cwd, tool::ToolContext& ctx) {
    if (git_stdout(cwd, {"rev-parse", "--is-inside-work-tree"}) != "true") {
        return;
    }
    ctx.git_branch = git_stdout(cwd, {"rev-parse", "--abbrev-ref", "HEAD"});
    ctx.git_repo_root = git_stdout(cwd, {"rev-parse", "--show-toplevel"});
    ctx.git_has_uncommitted = !git_stdout(cwd, {"status", "--porcelain"}).empty();
}

/// @brief #30：从对话历史构建只读历史摘要
/// @details 收集最近若干条非空 user 消息（时间正序），单条与总量均截断，避免冲刷上下文。
/// @param messages 对话历史
/// @return 历史摘要文本（无 user 消息时为空串）
std::string build_history_summary(const std::vector<ChatMessage>& messages) {
    constexpr size_t kMaxUserMsgs = 10;      ///< 最多纳入的 user 消息条数
    constexpr size_t kMaxLineChars = 200;    ///< 单条消息截断长度
    constexpr size_t kMaxTotalChars = 2000;  ///< 摘要总长度上限

    std::vector<std::string> lines;
    for (auto it = messages.rbegin(); it != messages.rend() && lines.size() < kMaxUserMsgs; ++it) {
        if (it->role != ChatMessage::Role::User || it->content.empty()) {
            continue;
        }
        std::string line = it->content;
        if (line.size() > kMaxLineChars) {
            line.resize(kMaxLineChars);
            line += "...";
        }
        lines.push_back(std::move(line));
    }
    std::reverse(lines.begin(), lines.end());  // 恢复时间正序

    std::string out;
    size_t total = 0;
    for (auto& line : lines) {
        if (!out.empty()) out += "\n";
        out += line;
        total += line.size();
        if (total >= kMaxTotalChars) {
            if (out.size() > kMaxTotalChars) out.resize(kMaxTotalChars);
            break;
        }
    }
    return out;
}

}  // anonymous namespace

// ============================================================
// 构造
// ============================================================

ReActLoop::ReActLoop(ICompletionProvider* provider, std::shared_ptr<tool::ToolRegistry> registry,
                     Config config, IConfigManager* config_manager, ITaskManager* task_manager,
                     std::string cwd, CacheAwareCompactor* external_compactor, IEventBus* event_bus,
                     skill::TouchCollector* touch_collector,
                     std::function<void()> file_index_invalidator, std::string session_id,
                     std::function<void(std::vector<ChatMessage>&)> queue_inject_cb)
    : m_provider(provider),
      m_registry(std::move(registry)),
      m_config(config),
      m_owned_compactor(external_compactor
                            ? nullptr
                            : std::make_unique<CacheAwareCompactor>(m_config.compactor_cfg)),
      m_compactor(external_compactor ? *external_compactor : *m_owned_compactor),
      m_config_manager(config_manager),
      m_task_manager(task_manager),
      m_event_bus(event_bus),
      m_cwd(std::move(cwd)),
      m_session_id(std::move(session_id)),
      m_touch_collector(touch_collector),
      m_file_index_invalidator(std::move(file_index_invalidator)),
      m_queue_inject_cb(std::move(queue_inject_cb)) {
    // issue #15-F: 构造函数不变量从 assert 改为 throw，避免 Debug 构建直接 abort
    // 构造失败抛 std::invalid_argument 是 C++ 标准模式，调用方可用 try/catch 处理
    if (provider == nullptr) {
        throw std::invalid_argument("ReActLoop: provider must not be null");
    }
    if (m_config_manager == nullptr) {
        throw std::invalid_argument(
            "ReActLoop: config_manager must not be null (H-5: explicit DI required)");
    }
    // cwd 为空时回退到进程当前目录（会话启动时捕获更稳定，避免运行中 cwd 漂移）
    if (m_cwd.empty()) {
        m_cwd = std::filesystem::current_path().string();
    }
    if (m_registry) {
        m_executor = std::make_unique<tool::ToolExecutor>(m_registry);
    }
}

// ============================================================
// build_request — 构建 CompletionRequest
// ============================================================

CompletionRequest ReActLoop::build_request(const std::vector<ChatMessage>& messages,
                                           const std::string& system_prompt,
                                           const nlohmann::json& tools_schema) const {
    CompletionRequest request;
    request.stream = true;

    if (!system_prompt.empty()) {
        request.messages.push_back(ChatMessage::system(system_prompt));
    }

    for (const auto& msg : messages) {
        request.messages.push_back(msg);
    }

    // 注入工具 schema（启用 function calling）
    // DS_CACHE M-2：按 function.name 排序后再赋值，消除注册顺序抖动导致的缓存击穿。
    // 与 prefix_shape::normalize_tools_schema 复用同一逻辑，保证发送字节与 hash 计算一致。
    if (!tools_schema.is_null() && tools_schema.is_array() && !tools_schema.empty()) {
        request.tools = normalize_tools_schema(tools_schema);
    }

    return request;
}

// ============================================================
// execute_thought — 流式读取 LLM 响应
// ============================================================

ReActLoop::ThoughtResult ReActLoop::execute_thought(const CompletionRequest& request,
                                                    const std::atomic<bool>& should_cancel,
                                                    TokenCallback on_token) {
    ThoughtResult result;

    // 提交推理请求
    auto reader = m_provider->submit_completion(request);
    if (!reader) {
        result.status = ThoughtResult::Error;
        return result;
    }

    // 流式累积 tool_use 的 input JSON
    struct PendingToolUse {
        std::string id;
        std::string name;
        std::string input_json;
    };
    std::vector<PendingToolUse> pending_tools;

    StreamChunk chunk;

    while (true) {
        if (should_cancel) {
            reader->cancel();
            result.status = ThoughtResult::Cancelled;
            break;
        }

        auto state = reader->next([&should_cancel]() { return should_cancel.load(); }, chunk);

        if (state == StreamState::HasData) {
            // 文本/推理增量
            if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                result.content += chunk.content_delta;
                result.reasoning += chunk.reasoning_delta;

                if (on_token) {
                    on_token(chunk.content_delta, chunk.reasoning_delta);
                }
            }

            // tool_use content_block 开始
            if (chunk.is_tool_use_start) {
                pending_tools.push_back({chunk.tool_use_id, chunk.tool_name, ""});
            }

            // tool_use input JSON 增量（流式拼接）
            if (chunk.is_tool_use_delta && !pending_tools.empty()) {
                pending_tools.back().input_json += chunk.tool_input_delta;
            } else if (chunk.is_tool_use_delta) {
                // 容错：模型未发送 tool_use_start 直接发送 arguments
                // （部分兼容模型如 Gemma 可能省略 id/name 首帧）
                pending_tools.push_back({"", "", chunk.tool_input_delta});
            }
        } else if (state == StreamState::Complete) {
            // 最后一个 chunk 可能携带 token 统计
            if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                result.content += chunk.content_delta;
                result.reasoning += chunk.reasoning_delta;

                if (on_token) {
                    on_token(chunk.content_delta, chunk.reasoning_delta);
                }
            }
            result.prompt_tokens = chunk.prompt_tokens;
            result.generated_tokens = chunk.generated_tokens;
            result.cache_creation_input_tokens = chunk.cache_creation_input_tokens;
            result.cache_read_input_tokens = chunk.cache_read_input_tokens;
            result.prompt_cache_hit_tokens = chunk.prompt_cache_hit_tokens;
            result.prompt_cache_miss_tokens = chunk.prompt_cache_miss_tokens;
            result.prompt_ms = chunk.prompt_ms;
            result.generation_ms = chunk.generation_ms;
            result.status = ThoughtResult::Completed;
            break;
        } else if (state == StreamState::Error) {
            result.status = ThoughtResult::Error;
            break;
        } else if (state == StreamState::Cancelled) {
            result.status = ThoughtResult::Cancelled;
            break;
        }
    }

    // 将 pending_tools 转换为 ToolUse
    for (const auto& ptu : pending_tools) {
        ToolUse tu;
        tu.id = ptu.id;
        tu.name = ptu.name;
        try {
            tu.input = ptu.input_json.empty() ? nlohmann::json::object()
                                              : nlohmann::json::parse(ptu.input_json);
        } catch (const nlohmann::json::parse_error&) {
            tu.input = nlohmann::json::object();
        }
        result.tool_uses.push_back(std::move(tu));
    }

    // Fallback：若模型未走标准 tool_calls 协议（部分本地推理后端如 lm-studio /
    // llama.cpp 在 auto 模式下会用文本内嵌 JSON 描述工具调用），从 content 中
    // 扫描形如 {"name":"Write","arguments":{...}} 或 {"tool":"Write","input":{...}}
    // 的 JSON 块解析为 ToolUse
    if (result.tool_uses.empty() && !result.content.empty()) {
        parse_embedded_tool_calls(result.content, result.tool_uses);
    }

    return result;
}

// ============================================================
// parse_embedded_tool_calls — 文本内嵌工具调用 fallback 解析
// ============================================================

void ReActLoop::parse_embedded_tool_calls(const std::string& content,
                                          std::vector<ToolUse>& out_tools) {
    out_tools.clear();

    // 扫描 content 中的每个 '{' 尝试解析为 JSON 对象
    // 识别 { "name": "Write", "arguments": {...} } 或 { "tool": "Write", "input": {...} }
    // 兼容 GLM / Qwen / Llama 等 GGUF 模型常见内嵌格式
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] != '{') continue;

        // 尝试从此处解析 JSON，需要找到匹配的 '}'
        int depth = 0;
        size_t j = i;
        bool in_string = false;
        bool escape = false;
        for (; j < content.size(); ++j) {
            char c = content[j];
            if (escape) {
                escape = false;
                continue;
            }
            if (c == '\\') {
                escape = true;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                continue;
            }
            if (in_string) continue;
            if (c == '{')
                depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) break;  // 闭合
            }
        }
        if (depth != 0) continue;  // 未配对，跳过

        std::string json_str = content.substr(i, j - i + 1);
        nlohmann::json obj;
        try {
            obj = nlohmann::json::parse(json_str);
        } catch (...) {
            continue;  // 不是合法 JSON，跳过
        }
        if (!obj.is_object()) continue;

        // 识别工具名字段：name / tool / function
        std::string tool_name = obj.value("name", "");
        if (tool_name.empty()) tool_name = obj.value("tool", "");
        if (tool_name.empty()) {
            // OpenAI 风格：{"type":"function","function":{"name":"...","arguments":"..."}}
            if (obj.contains("function") && obj["function"].is_object()) {
                tool_name = obj["function"].value("name", "");
            }
        }
        if (tool_name.empty()) {
            // 可能有 reasoning text 中的 JSON，但不是 tool 调用，跳过
            continue;
        }

        // 识别参数字段：arguments / input / parameters
        nlohmann::json tool_input;
        if (obj.contains("arguments")) {
            const auto& args = obj["arguments"];
            if (args.is_string()) {
                // arguments 可能是 JSON 字符串
                try {
                    tool_input = nlohmann::json::parse(args.get<std::string>());
                } catch (...) {
                    tool_input = nlohmann::json::object();
                }
            } else if (args.is_object()) {
                tool_input = args;
            }
        } else if (obj.contains("input") && obj["input"].is_object()) {
            tool_input = obj["input"];
        } else if (obj.contains("parameters") && obj["parameters"].is_object()) {
            tool_input = obj["parameters"];
        } else if (obj.contains("function") && obj["function"].is_object()) {
            const auto& func = obj["function"];
            if (func.contains("arguments")) {
                const auto& args = func["arguments"];
                if (args.is_string()) {
                    try {
                        tool_input = nlohmann::json::parse(args.get<std::string>());
                    } catch (...) {
                        tool_input = nlohmann::json::object();
                    }
                } else if (args.is_object()) {
                    tool_input = args;
                }
            }
        } else {
            tool_input = nlohmann::json::object();
        }

        if (!tool_input.is_object()) tool_input = nlohmann::json::object();

        // 排除明显不是工具调用的误识别（如 tool_name 为 "type" 等保留字）
        // 要求工具名至少 2 个字符且首字符为字母
        if (tool_name.size() < 2 || !std::isalpha(static_cast<unsigned char>(tool_name[0]))) {
            continue;
        }

        ToolUse tu;
        tu.id = "embedded_" + std::to_string(out_tools.size());
        tu.name = std::move(tool_name);
        tu.input = std::move(tool_input);
        out_tools.push_back(std::move(tu));
    }
}

// ============================================================
// normalize_tool_input — 工具输入规范化签名
// ============================================================

std::string ReActLoop::normalize_tool_input(const nlohmann::json& input) {
    // 只保留结构：对象键按名排序、数组按序、标量紧凑序列化。
    // 忽略键顺序与空白，使等价调用（即便参数顺序抖动）签名一致，用于停滞判定。
    std::string out;
    std::function<void(const nlohmann::json&)> append = [&](const nlohmann::json& j) {
        if (j.is_object()) {
            out += '{';
            std::vector<std::string> keys;
            for (auto it = j.begin(); it != j.end(); ++it) keys.push_back(it.key());
            std::sort(keys.begin(), keys.end());
            for (size_t i = 0; i < keys.size(); ++i) {
                if (i) out += ',';
                out += keys[i];
                out += ':';
                append(j.at(keys[i]));
            }
            out += '}';
        } else if (j.is_array()) {
            out += '[';
            for (size_t i = 0; i < j.size(); ++i) {
                if (i) out += ',';
                append(j[i]);
            }
            out += ']';
        } else {
            out += j.dump();  // 标量紧凑序列化（含字符串转义、数字规范化）
        }
    };
    append(input.is_null() ? nlohmann::json::object() : input);
    return out;
}

// ============================================================
// run_reviewer — 内部评审器（停滞/达上限时判断"是否继续"）
// ============================================================

ReActLoop::ReviewerDecision ReActLoop::run_reviewer(const std::string& user_request,
                                                    const std::vector<std::string>& tool_history,
                                                    int iteration, int remaining_budget,
                                                    bool at_limit) const {
    ReviewerDecision decision;  // 默认 continue_loop=false（wrap_up），失败即收尾
    if (!m_config.review_enabled) {
        return decision;
    }

    const std::string kSys =
        "你是严格的任务进度评审器，用最短判断决定 Agent 是否应继续投入推理。\n"
        "只输出一行 JSON 对象，不要任何多余文本或解释：\n"
        "{\"action\": \"continue\" 或 \"wrap_up\", \"reason\": \"不超过30字\"}\n"
        "- action=continue：任务尚未完成且修复方向明确；reason 给一句具体纠偏/下一步\n"
        "- action=wrap_up：已取得足够进展、或明显走偏/停滞、或继续性价比低；reason 给进展小结\n"
        "注意：不要因不确定就轻易 wrap_up；但若在重复做同一件事，倾向 wrap_up。";

    std::string ctx =
        "用户目标/请求：\n" +
        (user_request.empty() ? std::string("(由前文对话上下文决定)") : user_request) + "\n\n";
    if (!tool_history.empty()) {
        ctx += "最近工具执行序列：\n";
        for (const auto& line : tool_history) ctx += "- " + line + "\n";
        ctx += "\n";
    }
    ctx += std::format("当前进度：已用迭代 {} 轮，剩余基础预算 {} 轮。\n", iteration,
                       remaining_budget);
    ctx += at_limit ? "触发原因：已到达最大迭代预算。\n"
                    : "触发原因：检测到重复的工具调用（可能陷入循环）。\n";

    CompletionRequest req;
    req.stream = false;
    req.temperature = 0.0f;
    req.max_tokens = 200;
    req.messages.push_back(ChatMessage::system(kSys));
    req.messages.push_back(ChatMessage::user(ctx));

    auto reader = m_provider->submit_completion(req);
    if (!reader) {
        LOG_WARN("[react_loop] reviewer: submit failed, default wrap_up");
        return decision;
    }

    std::string text;
    StreamChunk chunk;
    while (true) {
        StreamState state = reader->next([]() { return false; }, chunk);
        if (state == StreamState::HasData || state == StreamState::Complete) {
            text += chunk.content_delta;
            if (state == StreamState::Complete) break;
        } else {
            break;  // Error / Cancelled
        }
    }

    const auto lbrace = text.find('{');
    const auto rbrace = text.rfind('}');
    if (lbrace != std::string::npos && rbrace != std::string::npos && rbrace > lbrace) {
        try {
            const nlohmann::json j =
                nlohmann::json::parse(text.substr(lbrace, rbrace - lbrace + 1));
            if (j.is_object()) {
                const std::string action = j.value("action", "");
                const std::string reason = j.value("reason", "");
                if (action == "continue") {
                    decision.continue_loop = true;
                    decision.correction = reason;
                } else {
                    decision.continue_loop = false;
                    decision.wrap_summary = reason;
                }
            }
        } catch (...) {
            LOG_WARN("[react_loop] reviewer: failed to parse decision, default wrap_up");
        }
    } else {
        LOG_WARN("[react_loop] reviewer: no JSON decision in response, default wrap_up");
    }

    if (decision.continue_loop && decision.correction.empty()) {
        decision.correction =
            "检测到重复动作，请回顾当前进展并换一条更高效的路径完成目标，不要再次调用相同工具做同一"
            "件事。";
    }

    LOG_INFO("[react_loop] reviewer decision: {}, reason='{}'",
             decision.continue_loop ? "continue" : "wrap_up",
             decision.continue_loop ? decision.correction : decision.wrap_summary);
    return decision;
}

// ============================================================
// run — ReAct 主循环
// ============================================================

ReActResult ReActLoop::run(std::vector<ChatMessage>& messages, const std::string& system_prompt,
                           const nlohmann::json& tools_schema,
                           const std::atomic<bool>& should_cancel, IReActObserver* observer) {
    // 3.2：将 IReActObserver 适配为 StepCallback + TokenCallback
    StepCallback on_step = nullptr;
    TokenCallback on_token = nullptr;

    if (observer) {
        on_step = [observer](const ReActStep& step) {
            switch (step.type) {
                case ReActStepType::Thought:
                    observer->on_thought(step);
                    break;
                case ReActStepType::Action:
                    observer->on_action(step);
                    break;
                case ReActStepType::Observation:
                    observer->on_observation(step);
                    break;
                case ReActStepType::FinalAnswer:
                    observer->on_final_answer(step);
                    break;
            }
        };
        on_token = [observer](const std::string& content_delta,
                              const std::string& reasoning_delta) {
            observer->on_token(content_delta, reasoning_delta);
        };
    }

    return run(messages, system_prompt, tools_schema, should_cancel, std::move(on_step),
               std::move(on_token));
}

// ============================================================
// run — ReAct 主循环（回调版本）
// ============================================================

ReActResult ReActLoop::run(std::vector<ChatMessage>& messages, const std::string& system_prompt,
                           const nlohmann::json& tools_schema,
                           const std::atomic<bool>& should_cancel, StepCallback on_step,
                           TokenCallback on_token) {
    ReActResult result;
    auto loop_start = std::chrono::steady_clock::now();
    int step_counter = 0;

    // #30：turn 级轻量环境感知——request_id / model / 历史摘要，turn 内复用（无子进程 I/O）。
    //      git 环境探测较重（需 fork 子进程），推迟到首个工具执行前的 Action 阶段懒加载，
    //      避免在无工具调用的 turn 或取消时序敏感场景下阻塞 run() 启动。
    const std::string turn_request_id = core::util::generate_uuid();
    const std::string turn_model =
        m_config_manager->get_or<std::string>(agent::keys::MODEL_NAME, "");
    const std::string turn_history_summary = build_history_summary(messages);
    // git 探测结果（首次用到工具时懒加载；非仓库时保持默认值）
    tool::ToolContext turn_env_probe;
    bool turn_git_probed = false;

    // 0.6.x：停滞检测 + 内部评审器。budget 为当前剩余预算（base 在"达上限评审→继续"时可追加）。
    // 用 while+budget 而非 for(m<=max) 表达，使"超限评审→追加预算"无需 goto 即可续跑同一循环体。
    int budget = std::max(1, m_config.max_iterations);
    int iteration = 1;
    int review_grants = 0;             ///< 达上限评审"继续"已允许的次数
    bool graceful_stop = false;        ///< 内部评审 wrap_up 的优雅收尾（非硬错误）
    bool hard_budget_reached = false;  ///< 预算(含追加)真正耗尽且未产出 final_answer
    std::deque<ToolCallSignature> recent_calls;  ///< 停滞判定滑动窗口（记录最近已执行调用签名）
    std::vector<std::string> tool_history;  ///< 最近工具执行日志（评审喂入）
    constexpr size_t kMaxToolHistory = 6;   ///< 喂给评审器的工具日志条数上限

    // 评审上下文用"用户主要任务"（第一条非空 user 消息，截断）而非全量历史
    std::string primary_request;
    for (const auto& m : messages) {
        if (m.role == ChatMessage::Role::User && !m.content.empty()) {
            primary_request = m.content;
            if (primary_request.size() > 400) {
                primary_request.resize(400);
                primary_request += "...";
            }
            break;
        }
    }

    while (!graceful_stop && budget > 0) {
        result.total_iterations = iteration;

        // ================================================================
        // === Thought 阶段 ===
        // ================================================================

        auto thought_start = std::chrono::steady_clock::now();

        LOG_INFO("[react_loop] iteration={} thought_begin, messages={}, has_tools={}", iteration,
                 messages.size(),
                 (!tools_schema.is_null() && tools_schema.is_array() && !tools_schema.empty()));

        // DS_CACHE M-6：压缩点移至 turn 间（仅 iteration == 1 时执行）
        // 原实现每 iteration > 1 都调用，会在单 turn 多步推理中过早截短前序 tool_result。
        // Plan 意图是 turn 间压缩：每 turn 开始时压缩一次，避免 turn 内工具链被破坏。
        // compactor 状态由 ChatSession 持有跨 turn 持久化（H-3）。
        if (iteration == 1) {
            auto compact_result = m_compactor.maybe_compact(messages);
            if (compact_result.action != CacheAwareCompactor::Action::None &&
                compact_result.action != CacheAwareCompactor::Action::SoftNotice) {
                LOG_INFO(
                    "[react_loop] turn-start compact action={}, snipped={}, folded={}, "
                    "tokens {} -> {}, rewrite_version={}",
                    static_cast<int>(compact_result.action), compact_result.snipped_count,
                    compact_result.compacted_count, compact_result.tokens_before,
                    compact_result.tokens_after, m_compactor.rewrite_version());
            }
        }

        CompletionRequest request = build_request(messages, system_prompt, tools_schema);
        ThoughtResult thought = execute_thought(request, should_cancel, on_token);

        auto thought_end = std::chrono::steady_clock::now();
        double thought_ms =
            std::chrono::duration<double, std::milli>(thought_end - thought_start).count();
        // 思考阶段实际耗时累计（UI 思考折叠标签 + 会话持久化 reasoningMs 用）
        result.reasoning_ms += thought_ms;

        LOG_INFO(
            "[react_loop] iteration={} thought_end, status={}, content_len={}, "
            "reasoning_len={}, tool_uses={}, prompt_tokens={}, generated_tokens={}, "
            "cache_creation={}, cache_read={}, prompt_ms={:.1f}, generation_ms={:.1f}, "
            "thought_ms={:.1f}",
            iteration, static_cast<int>(thought.status), thought.content.size(),
            thought.reasoning.size(), thought.tool_uses.size(), thought.prompt_tokens,
            thought.generated_tokens, thought.cache_creation_input_tokens,
            thought.cache_read_input_tokens, thought.prompt_ms, thought.generation_ms, thought_ms);

        // --- 处理 Thought 异常状态 ---

        if (thought.status == ThoughtResult::Cancelled) {
            LOG_WARN("[react_loop] iteration={} thought cancelled by user", iteration);
            result.was_interrupted = true;
            result.partial_content = thought.content;
            result.partial_reasoning = thought.reasoning;
            break;
        }

        if (thought.status == ThoughtResult::Error) {
            LOG_ERROR("[react_loop] iteration={} thought stream error", iteration);
            result.was_error = true;
            result.error_message = "Stream error during Thought phase";
            result.partial_content = thought.content;
            result.partial_reasoning = thought.reasoning;
            break;
        }

        // --- 记录 Thought 步骤 ---
        {
            ReActStep step;
            step.type = ReActStepType::Thought;
            step.step_number = ++step_counter;
            step.thought_text = thought.content;
            step.reasoning = thought.reasoning;
            step.tool_uses = thought.tool_uses;
            step.duration_ms = thought_ms;
            result.steps.push_back(step);

            if (on_step) {
                on_step(step);
            }
        }

        // 保存 token 统计（每次 Thought 更新，最终保留最后一次）
        result.prompt_tokens = thought.prompt_tokens;
        result.generated_tokens = thought.generated_tokens;
        result.cache_creation_input_tokens = thought.cache_creation_input_tokens;
        result.cache_read_input_tokens = thought.cache_read_input_tokens;
        result.prompt_cache_hit_tokens = thought.prompt_cache_hit_tokens;
        result.prompt_cache_miss_tokens = thought.prompt_cache_miss_tokens;
        result.prompt_ms = thought.prompt_ms;
        result.generation_ms = thought.generation_ms;

        // ================================================================
        // === 终止判断：无 tool_use → FinalAnswer ===
        // ================================================================

        if (thought.tool_uses.empty()) {
            // LLM 给出最终回复，无需工具调用
            LOG_INFO("[react_loop] iteration={} final_answer, content_len={}", iteration,
                     thought.content.size());
            messages.push_back(ChatMessage::assistant(thought.content));
            if (!thought.reasoning.empty()) {
                messages.back().reasoning_content = thought.reasoning;
            }

            result.final_answer = thought.content;
            result.final_reasoning = thought.reasoning;

            // 记录 FinalAnswer 步骤
            {
                ReActStep step;
                step.type = ReActStepType::FinalAnswer;
                step.step_number = ++step_counter;
                step.thought_text = thought.content;
                step.reasoning = thought.reasoning;
                step.duration_ms = thought_ms;
                result.steps.push_back(step);

                if (on_step) {
                    on_step(step);
                }
            }

            break;  // 正常退出循环
        }

        // ================================================================
        // === 停滞检测：重复工具调用（进入 Action 前判定）===
        // ================================================================
        if (m_config.review_enabled && m_config.review_stall_window > 1) {
            bool stall = false;
            for (const auto& tu : thought.tool_uses) {
                ToolCallSignature sig{tu.name, normalize_tool_input(tu.input)};
                if (std::find(recent_calls.begin(), recent_calls.end(), sig) !=
                    recent_calls.end()) {
                    stall = true;
                    break;
                }
            }

            if (stall) {
                LOG_WARN("[react_loop] iteration={} stall detected (repeated tool call)",
                         iteration);
                ReviewerDecision dec = run_reviewer(primary_request, tool_history, iteration,
                                                    budget, /*at_limit*/ false);
                if (dec.continue_loop) {
                    // 纠偏续跑：不执行重复工具（避免悬空 tool_calls），注入指令进入下轮 Thought
                    messages.push_back(ChatMessage::system(dec.correction));
                    recent_calls.clear();
                    --budget;
                    hard_budget_reached = (budget <= 0);  // 停滞连吃预算耗尽也判硬错误
                    ++iteration;
                    LOG_INFO("[react_loop] stall recovery: continue (budget={})", budget);
                    continue;  // 回到 while 条件，下一轮 Thought 重新规划
                }
                // wrap_up：优雅收尾（非硬错误）
                graceful_stop = true;
                result.final_answer = dec.wrap_summary.empty()
                                          ? "任务因检测到工具循环而由内部评审器中止。"
                                          : dec.wrap_summary;
                result.final_reasoning = "stall-recovery wrap up";
                LOG_WARN("[react_loop] stall recovery: wrap_up");
                break;
            }

            // 未停滞：把本次 tool_uses 压入窗口并截断
            for (const auto& tu : thought.tool_uses) {
                recent_calls.push_back({tu.name, normalize_tool_input(tu.input)});
            }
            while (static_cast<int>(recent_calls.size()) > m_config.review_stall_window) {
                recent_calls.pop_front();
            }
        }

        // ================================================================
        // === 有 tool_use：构建 assistant 消息 ===
        // ================================================================

        ChatMessage assistant_msg = ChatMessage::assistant(thought.content);
        if (!thought.reasoning.empty()) {
            assistant_msg.reasoning_content = thought.reasoning;
        }
        for (auto& tu : thought.tool_uses) {
            assistant_msg.tool_uses.push_back(tu);
        }
        messages.push_back(std::move(assistant_msg));

        // ================================================================
        // === Action + Observation 阶段 ===
        // ================================================================

        // 无 executor 时：以错误作为 tool_result 回传
        if (!m_executor) {
            for (const auto& tu : thought.tool_uses) {
                // Action 步骤
                ReActStep action_step;
                action_step.type = ReActStepType::Action;
                action_step.step_number = ++step_counter;
                action_step.tool_name = tu.name;
                action_step.tool_use_id = tu.id;
                action_step.tool_input = tu.input;
                result.steps.push_back(action_step);
                if (on_step) on_step(action_step);

                // Observation 步骤
                std::string err_msg = "Error: tool executor not configured";
                messages.push_back(ChatMessage::tool_result(tu.id, tu.name, err_msg, true));

                ReActStep obs_step;
                obs_step.type = ReActStepType::Observation;
                obs_step.step_number = ++step_counter;
                obs_step.tool_use_id = tu.id;
                obs_step.observation = err_msg;
                obs_step.is_error = true;
                result.steps.push_back(obs_step);
                if (on_step) on_step(obs_step);

                result.total_tool_calls++;
            }
            // 工具轮边界：冲刷排队用户消息（Ctrl+Enter）——注入下一轮 Thought 一并发送
            maybe_inject_queue(messages);
            --budget;
            ++iteration;
            continue;  // 继续下一轮 Thought（无执行器以错误回传，预算照常消耗）
        }

        // 有 executor：3.1 并行执行所有 tool_use
        // Phase 3 已审计：所有工具 call() const，无实例可变状态，可安全并行
        LOG_INFO("[react_loop] iteration={} action_begin, parallel_tools={}", iteration,
                 thought.tool_uses.size());

        tool::ToolContext ctx;
        ctx.cwd = m_cwd;  // 使用会话启动时捕获的 cwd，避免运行中 cwd 漂移
        // #30：首次执行工具时懒加载 git 环境探测（turn 内仅一次），避免阻塞 run() 启动
        if (!turn_git_probed) {
            detect_git_env(m_cwd, turn_env_probe);
            turn_git_probed = true;
        }
        // #30：注入真实会话 ID（否则回退 "default"），供审计日志按会话关联
        ctx.session_id = m_session_id.empty() ? std::string("default") : m_session_id;
        // #30：注入 turn 级 request_id（每次 turn 生成一次，审计/缓存链路关联）
        ctx.request_id = turn_request_id;
        // #30：注入当前模型名（来自 provider 配置），工具可按模型能力调整策略
        ctx.model = turn_model;
        // #30：注入 git 环境字段，工具改前可做分支/未提交风险提示
        ctx.git_branch = turn_env_probe.git_branch;
        ctx.git_has_uncommitted = turn_env_probe.git_has_uncommitted;
        ctx.git_repo_root = turn_env_probe.git_repo_root;
        // #30：注入只读历史摘要，工具可见用户早先约束（无修改历史的通道）
        ctx.history_summary = turn_history_summary;
        // 2.3 修复：将外部取消信号绑定到 ToolContext，工具可即时感知中断
        ctx.cancel_flag = &should_cancel;
        // H-5：注入配置管理器（非空），工具通过 ctx.config_manager() 访问
        ctx.config_manager_ptr = m_config_manager;
        // BashTool 后台任务 DI：注入任务管理器（可选）
        ctx.task_manager_ptr = m_task_manager;
        // AskUserTool 事件发布 DI：注入事件总线（可选）
        ctx.event_bus_ptr = m_event_bus;
        // conditional skills：注入 touch 回调（可选），工具上报访问过的文件
        if (m_touch_collector) {
            ctx.touch_callback = [collector = m_touch_collector](const std::string& path) {
                collector->add(path);
            };
        }
        // 宿主文件索引失效回调（可选）：FileWriteTool 写文件后通知宿主重建索引
        ctx.on_file_system_changed = m_file_index_invalidator;
        // #28：注入会话级权限模式 + 模式变更回调（EnterPlanMode/ExitPlanMode 工具切换）
        ctx.permission_mode = m_permission_mode;
        ctx.on_permission_mode_changed = [this](tool::PermissionMode mode) {
            m_permission_mode = mode;
            notify_permission_state();  // H-1：回写宿主，统一状态源
        };
        // 注入会话工作模式（极简模式白名单守卫依据）
        ctx.session_mode = m_session_mode;
        // #26：注入推理提供者 + 工具注册表（AgentTool 启动子 Agent 用）
        ctx.provider_ptr = m_provider;
        ctx.tool_registry = m_registry;
        // #50：注入循环级 HookManager，工具线程可触发 PermissionRequest / Subagent* 事件
        ctx.hook_manager_ptr = m_config.hooks;
        // #56 方案 C：注入命令注册表（AgentTool 子 Agent skill 预加载；可选）
        ctx.command_registry_ptr = m_command_registry.get();
        // #56 方案 D：注入会话级 MCP 连接管理器（工具访问可用 MCP server；可选）
        ctx.mcp_manager_ptr = m_mcp_manager.get();
        // #28 评审 #1/#3：进入计划模式——保存原模式、切换 Plan；Bypass 禁止降级、已在 Plan
        // 则幂等拒绝
        ctx.on_enter_plan_mode = [this]() -> bool {
            if (m_permission_mode == tool::PermissionMode::BypassPermissions) {
                return false;  // Bypass 全权模式禁止降级到 Plan
            }
            if (m_in_plan_mode) {
                return false;  // 已在 Plan，幂等
            }
            m_permission_mode_before_plan = m_permission_mode;
            m_permission_mode = tool::PermissionMode::Plan;
            m_in_plan_mode = true;
            notify_permission_state();  // H-1：回写宿主，统一状态源
            return true;
        };
        // #28 评审 #1：退出计划模式——恢复进入前的原模式，而非硬编码 Default
        ctx.on_exit_plan_mode = [this]() {
            if (m_in_plan_mode) {
                m_permission_mode = m_permission_mode_before_plan;
                m_in_plan_mode = false;
                notify_permission_state();  // H-1：回写宿主，统一状态源
            }
        };

        // 1. 同步发布所有 Action 步骤（UI 即时反馈工具调用开始）
        for (const auto& tu : thought.tool_uses) {
            ReActStep step;
            step.type = ReActStepType::Action;
            step.step_number = ++step_counter;
            step.tool_name = tu.name;
            step.tool_use_id = tu.id;
            step.tool_input = tu.input;
            result.steps.push_back(step);

            if (on_step) {
                on_step(step);
            }
        }

        // 2. Issue #50：PreToolUse 事件（并行执行前同步调度，可拦截危险操作）
        //    构造统一 hook 上下文；对任一 tool_use 命中 blockingError 的，
        //    跳过其执行并在 Observation 阶段注入 blocker 错误。
        std::vector<bool> blocked(thought.tool_uses.size(), false);
        std::vector<std::string> blocked_text(thought.tool_uses.size());
        if (m_config.hooks && !m_config.hooks->empty()) {
            for (size_t i = 0; i < thought.tool_uses.size(); ++i) {
                hook::HookContext hctx;
                hctx.session_id = m_session_id.empty() ? std::string("default") : m_session_id;
                hctx.cwd = m_cwd;
                hctx.request_id = turn_request_id;
                hctx.tool_name = thought.tool_uses[i].name;
                hctx.tool_input = thought.tool_uses[i].input;
                auto hres = m_config.hooks->dispatch(hook::HookEvent::PreToolUse, hctx);
                if (hres.blockingError && !hres.blockingError->empty()) {
                    blocked[i] = true;
                    blocked_text[i] = std::format("Error: tool '{}' blocked by PreToolUse hook: {}",
                                                  thought.tool_uses[i].name, *hres.blockingError);
                    LOG_WARN("[react_loop] PreToolUse blocked tool={}: {}",
                             thought.tool_uses[i].name, *hres.blockingError);
                }
            }
        }

        // 3. 异步并行执行所有 tool_use
        struct ToolExecution {
            std::string tool_use_id;
            std::string tool_name;
            std::future<std::pair<std::string, bool>> future;  // {result_text, is_error}
        };

        std::vector<ToolExecution> executions;
        executions.reserve(thought.tool_uses.size());

        for (size_t i = 0; i < thought.tool_uses.size(); ++i) {
            const auto& tu = thought.tool_uses[i];
            if (blocked[i]) {
                // 被 PreToolUse 拦截：构造【就绪】的 future（error/即时完成），
                // Observation 轮询 wait_for 才能立即返回。deferred future 的
                // wait_for 恒返回 deferred，会导致下方 while 死循环。
                std::promise<std::pair<std::string, bool>> ready_promise;
                ready_promise.set_value({blocked_text[i], true});
                executions.push_back({tu.id, tu.name, ready_promise.get_future()});
                continue;
            }
            LOG_DEBUG("[react_loop] launching async tool_use, id={}, name={}", tu.id, tu.name);
            auto future =
                std::async(std::launch::async, [this, &ctx, &tu]() -> std::pair<std::string, bool> {
                    try {
                        auto exec_result = m_executor->execute(tu.name, tu.input, ctx);
                        if (exec_result.is_err()) {
                            // V2-4：错误由 ResultV2 承载，message 用于 LLM 可读反馈
                            return {std::format("Error: {}", exec_result.error().message), true};
                        }
                        return {exec_result.value().result.to_string(), false};
                    } catch (const std::exception& e) {
                        return {
                            std::format("Error: tool '{}' threw exception: {}", tu.name, e.what()),
                            true};
                    } catch (...) {
                        return {std::format("Error: tool '{}' threw unknown exception", tu.name),
                                true};
                    }
                });

            executions.push_back({tu.id, tu.name, std::move(future)});
            result.total_tool_calls++;
        }

        // 4. 等待所有工具完成，按原始顺序生成 Observation
        for (auto& exec : executions) {
            auto action_start = std::chrono::steady_clock::now();

            // 协作式等待（#23 P1）：工具已绑定 ctx.cancel_flag=should_cancel，
            // 用户中断后工具会自己退出（bash 由 subprocess 杀进程树），
            // 这里用 wait_for 轮询替代阻塞的 future.get()，使打断即时生效，
            // 且 loop 在工具返回前不丢消息（Observation 照常先生成）。
            // 注意：不做超时脱离 —— std::async(future) 析构会隐式等待线程，
            //           孤儿线程不可真正 detach（残余风险记入 P3 保险丝）。
            while (exec.future.wait_for(std::chrono::milliseconds(100)) !=
                   std::future_status::ready) {
                if (should_cancel) {
                    LOG_DEBUG(
                        "[react_loop] tool={} waiting for cooperative "
                        "cancel (user interrupt)",
                        exec.tool_name);
                }
            }

            auto [result_text, tool_error] = exec.future.get();

            auto action_end = std::chrono::steady_clock::now();
            double action_ms =
                std::chrono::duration<double, std::milli>(action_end - action_start).count();

            // --- Observation 阶段 ---

            // 添加 tool_result 消息到对话历史
            messages.push_back(ChatMessage::tool_result(exec.tool_use_id, exec.tool_name,
                                                        result_text, tool_error));

            // Issue #50：PostToolUse 事件（工具执行后，含被 PreToolUse 拦截的场景）
            if (m_config.hooks && !m_config.hooks->empty()) {
                hook::HookContext hctx;
                hctx.session_id = m_session_id.empty() ? std::string("default") : m_session_id;
                hctx.cwd = m_cwd;
                hctx.request_id = turn_request_id;
                hctx.tool_name = exec.tool_name;
                hctx.tool_result = result_text;
                hctx.tool_error = tool_error;
                auto hres = m_config.hooks->dispatch(hook::HookEvent::PostToolUse, hctx);
                (void)hres;  // 本版本 PostToolUse 仅记录；blockingError 不在此事件生效
            }

            // 累计评审用工具历史（每条取观察首行，最多 kMaxToolHistory 条）
            {
                std::string firstline = result_text;
                const auto nl = firstline.find('\n');
                if (nl != std::string::npos) firstline.resize(nl);
                if (firstline.size() > 120) {
                    firstline.resize(120);
                    firstline += "...";
                }
                tool_history.push_back(exec.tool_name + ": " + firstline);
                if (tool_history.size() > kMaxToolHistory) {
                    tool_history.erase(tool_history.begin());
                }
            }

            // 记录 Observation 步骤
            {
                ReActStep step;
                step.type = ReActStepType::Observation;
                step.step_number = ++step_counter;
                step.tool_use_id = exec.tool_use_id;
                step.observation = result_text;
                step.is_error = tool_error;
                step.duration_ms = action_ms;
                result.steps.push_back(step);

                if (on_step) {
                    on_step(step);
                }
            }

            LOG_INFO("[react_loop] tool={} completed, is_error={}, duration={}ms", exec.tool_name,
                     tool_error, action_ms);
        }

        // 工具轮边界：冲刷排队用户消息（Ctrl+Enter）——注入下一轮 Thought 一并发送
        // 位于 Action + Observation 全部完成后、预算递减前
        maybe_inject_queue(messages);

        // 继续下一轮 Thought（LLM 根据 tool_result 决定下一步）
        --budget;

        // --- 达上限评审门：基础预算耗尽且本轮未产出 final_answer → 评审是否追加 ---
        if (budget <= 0 && m_config.review_enabled &&
            review_grants < std::max(0, m_config.review_max_grants)) {
            ReviewerDecision dec =
                run_reviewer(primary_request, tool_history, iteration, budget, /*at_limit*/ true);
            if (dec.continue_loop) {
                ++review_grants;
                budget += std::max(1, m_config.review_extra_budget);
                const std::string correction = dec.correction.empty()
                                                   ? "已到达默认迭代上限，请尽快收敛：若任务完成请"
                                                     "给出最终答复，否则换更高效的路径。"
                                                   : dec.correction;
                messages.push_back(ChatMessage::system(correction));
                LOG_WARN(
                    "[react_loop] base budget exhausted, reviewer grants extra {} "
                    "iterations (grant #{}), budget now {}",
                    m_config.review_extra_budget, review_grants, budget);
            } else {
                // wrap_up：优雅收尾（非硬错误）
                graceful_stop = true;
                result.final_answer =
                    dec.wrap_summary.empty()
                        ? std::format("已达到最大迭代预算（{} 轮）但任务未完成，由内部评审器中止。",
                                      m_config.max_iterations)
                        : dec.wrap_summary;
                result.final_reasoning = "at-limit wrap up";
                LOG_WARN("[react_loop] at-limit reviewer decided wrap_up");
                break;
            }
        }

        ++iteration;
        // 预算（含追加）耗尽且评审未授予继续 → 供循环后判硬错误
        hard_budget_reached = (budget <= 0);
    }

    // ================================================================
    // 循环结束处理
    // ================================================================

    auto loop_end = std::chrono::steady_clock::now();
    result.total_duration_ms =
        std::chrono::duration<double, std::milli>(loop_end - loop_start).count();

    // DS_CACHE H-2：回填压缩器改写版本号，供 ChatSession 传入 capture_shape
    // 使 compare_shape 的 log_rewrite 归因在 compact 改写历史后能正确触发
    result.rewrite_version = m_compactor.rewrite_version();

    // 超过预算（含评审追加）且未产出 final_answer：视为硬错误
    // 注意：LLM 返回空 content + 无 tool_use 时也会 break 退出，此时 final_answer 为空，
    // 但属于正常退出（LLM 主动结束），不应误判为 max iterations（hard_budget_reached=false）。
    if (hard_budget_reached && !result.was_interrupted && !result.was_error &&
        result.final_answer.empty()) {
        result.was_error = true;
        result.error_message =
            std::format("Agent loop reached max budget ({} iterations)", result.total_iterations);
        LOG_WARN(
            "[react_loop] max budget reached={}, total_duration_ms={:.1f}, "
            "total_tool_calls={}",
            result.total_iterations, result.total_duration_ms, result.total_tool_calls);
    } else {
        LOG_INFO(
            "[react_loop] loop end, iterations={}, total_duration_ms={:.1f}, "
            "total_tool_calls={}, was_interrupted={}, was_error={}, "
            "graceful_stop={}",
            result.total_iterations, result.total_duration_ms, result.total_tool_calls,
            result.was_interrupted, result.was_error, graceful_stop);
    }

    // Issue #50：Stop 事件（Agent 停止——统一在循环收尾后触发一次）
    //    blockingError → 注入 user 消息并终止继续；preventContinuation → 后续不继续
    if (m_config.hooks && !m_config.hooks->empty()) {
        hook::HookContext hctx;
        hctx.session_id = m_session_id.empty() ? std::string("default") : m_session_id;
        hctx.cwd = m_cwd;
        hctx.request_id = turn_request_id;
        hctx.final_answer = result.final_answer;
        if (result.was_interrupted)
            hctx.stop_reason = "interrupted";
        else if (result.was_error)
            hctx.stop_reason = "error";
        else if (graceful_stop)
            hctx.stop_reason = "at_limit";
        else
            hctx.stop_reason = "completed";
        auto hres = m_config.hooks->dispatch(hook::HookEvent::Stop, hctx);
        if (hres.blockingError && !hres.blockingError->empty()) {
            // 注入 user 角色修正消息（用户视角的阻断指引），并重置 continue 语义
            messages.push_back(ChatMessage::user(*hres.blockingError));
            result.was_error = true;
            result.error_message = *hres.blockingError;
            if (!result.final_answer.empty()) {
                result.final_answer = *hres.blockingError;
            }
            LOG_WARN("[react_loop] Stop hook blockingError injected: {}", *hres.blockingError);
        }
        if (hres.preventContinuation && !result.final_answer.empty()) {
            // 语义：停止后续 query 循环（本 turn 已结束，等效清空 final_answer 以防继续）
            LOG_WARN("[react_loop] Stop hook preventContinuation=true (turn already ended)");
        }
    }

    return result;
}

}  // namespace agent
