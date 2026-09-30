/**
 * @file app_config.cpp
 * @brief 应用配置加载实现
 * @details 配置 Schema 注册、环境变量加载、配置文件加载、默认路径
 * @version 2.0.1
 * @date 2026-07
 *
 * v2.0.0 变更（C-2/C-4）：
 *   - register_meta → register_schema（结构化类型/范围/枚举约束）
 *   - 6 个标准环境变量绑定到 Schema，由 ConfigManager::load_from_env() 统一加载
 *   - WORKX_NO_COLOR 保留 presence-only 语义（兼容 NO_COLOR 规范），手动处理
 */

#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <ctime>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>  // GetModuleFileNameW（Debug 日志路径用）
#else
#include <unistd.h>  // readlink（Debug 日志路径用）
#endif

#include "agent/config/app_config.h"
#include "core/config/config_manager.h"
#include "core/config/i_config_manager.h"
#include "agent/tool/constants.h"

namespace agent {

void register_config_defaults(ConfigManager& cfg) {
    // === Terminal ===
    cfg.register_schema({.key = keys::SIMPLE_IO,
                         .description = "Use simple I/O mode (getline)",
                         .default_value = false,
                         .type = ConfigSchema::Type::Bool});
    cfg.register_schema({
        .key = keys::NO_COLOR,
        .description = "Disable colored output (env WORKX_NO_COLOR: presence-only)",
        .default_value = false,
        .type = ConfigSchema::Type::Bool
        // env_var 留空：presence-only 语义需手动处理，见 load_from_env()
    });
    cfg.register_schema({.key = keys::VERBOSE,
                         .description = "Show verbose startup debug info",
                         .default_value = false,
                         .type = ConfigSchema::Type::Bool});
    cfg.register_schema({.key = keys::PROMPT,
                         .description = "Prompt string",
                         .default_value = std::string("> "),
                         .type = ConfigSchema::Type::String});

    // === Backend ===
    cfg.register_schema({.key = keys::REMOTE_URL,
                         .description = "Remote API base URL (OpenAI-compatible)",
                         .default_value = std::string(""),
                         .is_required = false,
                         .type = ConfigSchema::Type::String,
                         .env_var = "WORKX_BASE_URL"});
    cfg.register_schema({.key = keys::MODEL_NAME,
                         .description = "Model name for remote API",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String,
                         .env_var = "WORKX_MODEL"});
    cfg.register_schema({.key = keys::API_KEY,
                         .description = "API key for remote API",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String,
                         .env_var = "WORKX_API_KEY"});
    cfg.register_schema(
        {.key = keys::PROVIDER,
         // 注意：有意保持 Type::String（而非固定 Enum）。多供应商面板支持自定义
         // 供应商（手输名称/URL，id 为 "new-provider-N" 占位或任意自定义值），
         // 固定 6 值 Enum 会拒绝合法自定义 id 导致切换失败。
         // CLI 侧（--provider）已有 find_preset 白名单校验（cli_args.cpp）。
         .description =
             "Provider name (deepseek, glm, kimi, qwen, minimax, openai-compatible, or custom)",
         .default_value = std::string(""),
         .type = ConfigSchema::Type::String});
    cfg.register_schema({.key = keys::TIMEOUT_MS,
                         .description = "HTTP timeout in milliseconds",
                         .default_value = 30000,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(1, 86400000),  // 1ms ~ 24h
                         .env_var = "WORKX_TIMEOUT"});
    cfg.register_schema({.key = keys::CONTEXT_LENGTH,
                         .description = "Model context window (tokens), overrides preset default",
                         .default_value = 0,  // 0 表示未设置，由 provider preset 决定
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(0, 2000000)});
    cfg.register_schema(
        {.key = keys::SEND_REASONING,
         .description = "Send reasoning_content back to model (DeepSeek-reasoner CoT roundtrip, "
                        "increases prompt but may improve cache hit rate for multi-turn CoT)",
         .default_value = false,
         .type = ConfigSchema::Type::Bool});

    // === Retry ===
    cfg.register_schema({.key = keys::RETRY_COUNT,
                         .description = "Max retry attempts for transient errors",
                         .default_value = 3,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(0, 100)});
    cfg.register_schema({
        .key = keys::RETRY_DELAY_MS,
        .description = "Initial retry delay in ms (doubles each retry)",
        .default_value = 1000,
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(1, 3600000)  // 1ms ~ 1h
    });

    // === Session ===
    cfg.register_schema({.key = keys::SYSTEM_PROMPT,
                         .description = "System prompt for chat session",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String});
    cfg.register_schema({.key = keys::SAVE_PATH,
                         .description = "Default save path for /save command",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String});

    // === Agent ===
    cfg.register_schema(
        {.key = keys::AGENT_ACTIVE,
         .description = "Current agent type (empty=ReAct; goal-guarded/verify=GoalGuardedAgent; "
                        "planner/coordinator/researcher/reviewer=read-only/planning roles; "
                        "executor=execute; coordinator+AgentTool; batch/watch/script=no-LLM modes; "
                        "background/bg=run request in background, non-blocking, event-notified); "
                        "non-empty also filters active skills",
         .default_value = std::string(""),
         .type = ConfigSchema::Type::String});

    // 0.6.x：#31 目标导向 Agent 的目标声明（agent.active=goal-guarded/verify 时生效）
    cfg.register_schema({.key = keys::AGENT_GOAL,
                         .description =
                             "Goal for goal-guarded agent: tests_pass / build_clean / "
                             "lint_zero / file_exists:<path> / cmd:<command> (empty = no goal)",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String});

    // 0.6.x：ReAct 循环基础预算（最大迭代轮数）。默认 40（原硬编码 25）；
    // 预算耗尽或检测到重复工具调用时，内部评审器可评审"是否继续"并追加预算
    // （见 ReActLoop::Config::review_* 字段；此键仅控制基础预算）。
    cfg.register_schema({.key = keys::AGENT_MAX_ITERATIONS,
                         .description =
                             "Max ReAct iterations per turn (base budget; internal reviewer "
                             "may grant extra iterations when stall/limit is detected)",
                         .default_value = 40,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(1, 2000)});

    // #79：子 Agent 派生护栏。防递归已由"子 Agent 工具集不含 Agent 工具"保证，
    // 剩余风险是规模失控（一次传入上百个 tasks，或多轮累计派生），故设两级上限：
    // 单次批量规模 + 单次 run 累计总量。超限时 AgentTool 整批拒绝并回灌可读错误，
    // 由模型自行拆分/缩减/改为自己完成。0 或负 = 不限（不推荐）。
    cfg.register_schema({.key = keys::AGENT_SUB_AGENT_MAX_BATCH,
                         .description =
                             "Max sub-agents launched by a single Agent tool call "
                             "(0 = unlimited). Exceeding it rejects the whole batch with a "
                             "model-readable error so the agent can split or shrink the work.",
                         .default_value = 10,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(0, 1000)});
    cfg.register_schema({.key = keys::AGENT_SUB_AGENT_MAX_TOTAL,
                         .description =
                             "Max sub-agents cumulatively launched within one ReAct run "
                             "(0 = unlimited). Guards against unbounded fan-out across turns.",
                         .default_value = 50,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(0, 10000)});

    // === Plan Mode V2（#54：五阶段多 Agent 规划流程）===
    cfg.register_schema({.key = keys::PLAN_AUTO,
                         .description =
                             "Auto-run the plan stages (interview→explore→plan) on entering "
                             "plan mode. When false, only the legacy manual behavior remains.",
                         .default_value = true,
                         .type = ConfigSchema::Type::Bool});
    cfg.register_schema({.key = keys::PLAN_INTERVIEW_ENABLED,
                         .description =
                             "Enable the Interview stage: clarify requirements / collect "
                             "constraints before exploring the codebase.",
                         .default_value = true,
                         .type = ConfigSchema::Type::Bool});
    cfg.register_schema(
        {.key = keys::PLAN_EXPLORE_AGENT_COUNT,
         .description =
             "Number of explore agents to run in parallel (getPlanModeV2ExploreAgentCount)",
         .default_value = 3,
         .type = ConfigSchema::Type::Int,
         .int_range = std::make_pair<int64_t, int64_t>(1, 8)});
    cfg.register_schema({.key = keys::PLAN_EXPLORE_AREAS,
                         .description =
                             "Comma-separated subdomains to explore in parallel (empty = generate "
                             "generic prompts per explore agent)",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String});

    // === Hooks（#50 通用 Hook 事件系统）===
    cfg.register_schema({.key = keys::HOOKS_ENABLED,
                         .description =
                             "Enable the general hook event system (PreToolUse/PostToolUse/Stop "
                             "et al). When disabled, no HookManager is built and hooks never run.",
                         .default_value = true,
                         .type = ConfigSchema::Type::Bool});
    cfg.register_schema({
        .key = keys::HOOKS_DEFINITIONS,
        .description = "JSON string array of hook definitions, each matching HookDefinition: "
                       "{event,type,match,command,url,prompt,timeout_ms,once,...}",
        .default_value = std::string("[]"),
        .type = ConfigSchema::Type::String  // JSON 以字符串承载（schema 无原生 JSON 类型）
    });
    cfg.register_schema({.key = keys::HOOKS_TIMEOUT_MS,
                         .description = "Default hook execution timeout in milliseconds (per-hook "
                                        "timeout_ms overrides)",
                         .default_value = 30000,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(100, 3600000)});

    // === Logging ===
    cfg.register_schema({.key = keys::LOG_LEVEL,
                         .description = "Log level",
                         .default_value = std::string("info"),
                         .type = ConfigSchema::Type::Enum,
                         .enum_values = {"trace", "debug", "info", "warn", "error", "fatal"},
                         .env_var = "WORKX_LOG_LEVEL"});
    cfg.register_schema({.key = keys::LOG_FILE,
                         .description = "Log file path (empty to disable file logging)",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String,
                         .env_var = "WORKX_LOG_FILE"});
    cfg.register_schema(
        {.key = keys::LOG_RETENTION_DAYS,
         .description = "Retention days for legacy timestamped run logs workx_*.log (0 = keep all)",
         .default_value = 7,
         .type = ConfigSchema::Type::Int,
         .int_range = std::make_pair<int64_t, int64_t>(0, 3650)});
    cfg.register_schema(
        {.key = keys::LOG_MAX_SIZE_MB,
         .description =
             "Run log roll size in MB (0 = no rotation, single workx.log grows unbounded)",
         .default_value = 10,
         .type = ConfigSchema::Type::Int,
         .int_range = std::make_pair<int64_t, int64_t>(0, 1024)});
    cfg.register_schema(
        {.key = keys::LOG_MAX_FILES,
         .description = "Max rolled run log files kept besides workx.log (workx.log.1 .. .N)",
         .default_value = 5,
         .type = ConfigSchema::Type::Int,
         .int_range = std::make_pair<int64_t, int64_t>(1, 100)});

    // === Audit（#37 审计日志：大小轮转 + 天数清理）===
    cfg.register_schema({.key = keys::AUDIT_ENABLED,
                         .description = "Enable audit logging (tool invoke / security events)",
                         .default_value = true,
                         .type = ConfigSchema::Type::Bool});
    cfg.register_schema({.key = keys::AUDIT_FILE,
                         .description = "Audit log file path (empty = logs/audit.jsonl)",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String});
    cfg.register_schema({.key = keys::AUDIT_MAX_SIZE_MB,
                         .description = "Audit log rotation size in MB",
                         .default_value = 10,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(1, 1024)});
    cfg.register_schema({.key = keys::AUDIT_RETENTION_DAYS,
                         .description = "Audit log retention days (rotated files)",
                         .default_value = 30,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(1, 3650)});

    // === Tool — FileReadTool ===
    cfg.register_schema({
        .key = keys::FILE_READ_MAX_SIZE,
        .description = "Max file size in bytes for Read tool (default 2MB)",
        .default_value = static_cast<int>(agent::tool::constants::MAX_FILE_SIZE_BYTES),
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(1, 1024 * 1024 * 1024)  // 1B ~ 1GB
    });
    cfg.register_schema({.key = keys::FILE_READ_MAX_LINES,
                         .description = "Max lines to read per call for Read tool (default 2000)",
                         .default_value = agent::tool::constants::MAX_LINES_TO_READ,
                         .type = ConfigSchema::Type::Int,
                         .int_range = std::make_pair<int64_t, int64_t>(1, 1000000)});

    // === Tool — FileEditTool ===
    cfg.register_schema({.key = keys::EDIT_DENY_PATTERNS,
                         .description =
                             "Newline-separated glob patterns for paths denied by Edit tool "
                             "(e.g. \"~/.ssh/**\\n**/.env\\n**/.git/**\")",
                         .default_value = std::string(""),
                         .type = ConfigSchema::Type::String});
    cfg.register_schema(
        {.key = keys::EDIT_SCAN_SECRETS,
         .description = "Scan new_string for potential secrets before editing (default false)",
         .default_value = false,
         .type = ConfigSchema::Type::Bool});

    // === Island（灵动岛 GUI IPC）===
    cfg.register_schema(
        {.key = keys::ISLAND_ENABLED,
         .description = "Enable Island IPC server (GUI discovers this TUI via registry)",
         .default_value = true,
         .type = ConfigSchema::Type::Bool});
    cfg.register_schema(
        {.key = keys::ISLAND_USD_CNY_RATE,
         .description = "CNY to USD exchange rate for balance display (DeepSeek returns CNY)",
         .default_value = 7.2,
         .type = ConfigSchema::Type::Double});

    // === Web（#25 WebSearchTool / WebFetchTool）===
    cfg.register_schema({.key = keys::WEB_SEARCH_PROVIDER,
                         .description = "Web search provider: tavily (default) / serper / searxng "
                                        "(P1 chained fallback reserved)",
                         .default_value = std::string("tavily"),
                         .type = ConfigSchema::Type::String,
                         .env_var = "WORKX_SEARCH_PROVIDER"});
    cfg.register_schema(
        {.key = keys::WEB_SEARCH_TAVILY_KEY,
         .description = "Tavily API key for WebSearchTool (env TAVILY_API_KEY overrides)",
         .default_value = std::string(""),
         .type = ConfigSchema::Type::String,
         .env_var = "TAVILY_API_KEY"});
    cfg.register_schema({.key = keys::WEB_SEARCH_SEARXNG_URL,
                         .description = "SearXNG instance URL for keyless search fallback "
                                        "(default public instance; set your own for stability)",
                         .default_value = std::string("https://searx.be"),
                         .type = ConfigSchema::Type::String,
                         .env_var = "WORKX_SEARXNG_URL"});
}

void load_from_env(ConfigManager& cfg) {
    // 1. 由 ConfigManager 统一加载已绑定到 Schema 的环境变量
    //    覆盖：WORKX_API_KEY / WORKX_BASE_URL / WORKX_MODEL / WORKX_TIMEOUT
    //          / WORKX_LOG_LEVEL / WORKX_LOG_FILE
    cfg.load_from_env();

    // 2. WORKX_NO_COLOR 采用 presence-only 语义（兼容 https://no-color.org 规范）：
    //    环境变量存在且非空即启用 no_color，不依赖值解析
    if (const char* val = std::getenv("WORKX_NO_COLOR")) {
        if (val[0] != '\0') {
            cfg.set(keys::NO_COLOR, true);
        }
    }
}

void load_from_config_file(IConfigManager& cfg, const std::filesystem::path& path) {
    auto result = cfg.load_from_file(path);
    if (result.is_err()) {
        std::cerr << "Warning: " << result.error().to_string() << "\n";
    }
}

// F.5：统一配置目录解析，优先级链：
//   1. $WORKX_CONFIG_DIR 环境变量（用户/管理员显式指定）
//   2. $USERPROFILE (Windows) / $HOME (POSIX) → ~/.workx（对齐 cc 的 ~/.claude 风格）
//   3. $APPDATA (Windows) / $XDG_CONFIG_HOME (POSIX)（受限环境回退）
//   4. 当前工作目录（最后回退，避免从快捷方式启动时配置丢失）
static std::filesystem::path get_config_dir() {
    // 1. 显式环境变量优先
    if (const char* env = std::getenv("WORKX_CONFIG_DIR")) {
        if (env[0] != '\0') return std::filesystem::path(env);
    }
#ifdef _WIN32
    // 2. %USERPROFILE%\.workx（主选，对齐 cc 的 ~/.claude 风格）
    if (const char* env = std::getenv("USERPROFILE")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
    // 3. %APPDATA%\workx（USERPROFILE 缺失时的回退）
    if (const char* env = std::getenv("APPDATA")) {
        if (env[0] != '\0') return std::filesystem::path(env) / "workx";
    }
#else
    // 2. $HOME/.workx（主选）
    if (const char* env = std::getenv("HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
    // 3. $XDG_CONFIG_HOME/workx（HOME 缺失时的回退）
    if (const char* env = std::getenv("XDG_CONFIG_HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / "workx";
    }
#endif
    // 4. 最后回退：当前工作目录下的 .workx
    return std::filesystem::current_path() / ".workx";
}

std::filesystem::path default_config_path() { return get_config_dir() / "config.json"; }

std::filesystem::path default_log_path() {
    // 单一固定文件名 workx.log，按大小轮转为 workx.log.1/.2/...
    // 避免旧版每次启动生成 workx_YYYYMMDD_HHMMSS.log 导致文件无限堆积
    std::string log_filename = "workx.log";

    // 所有构建（Debug/Release）：日志统一写入用户配置目录 ~/.workx/logs/
    // 便于在多启动实例间集中管理日志，避免散落于 exe 同目录
    return get_config_dir() / "logs" / log_filename;
}

std::filesystem::path log_dir() { return get_config_dir() / "logs"; }

void cleanup_expired_logs(int retention_days) {
    if (retention_days <= 0) return;

    std::error_code ec;
    auto dir = get_config_dir() / "logs";
    if (!std::filesystem::is_directory(dir, ec)) return;

    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24LL * retention_days);

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        // 仅清理主运行日志的轮转文件（workx.log.N）与旧版时间戳日志
        // （workx_YYYYMMDD_HHMMSS.log），不影响活动文件 workx.log、
        // workx_tui.log / workx_crash.log / workx_audit.jsonl
        const std::string name = entry.path().filename().string();
        const bool is_rotated = name.rfind("workx.log.", 0) == 0;
        const bool is_legacy_ts = name.rfind("workx_", 0) == 0 && name.size() >= 5 &&
                                  name.compare(name.size() - 4, 4, ".log") == 0 &&
                                  name != "workx_tui.log" && name != "workx_crash.log";
        if (!is_rotated && !is_legacy_ts) {
            continue;
        }

        auto mtime = std::filesystem::last_write_time(entry.path(), ec);
        if (ec) continue;
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            mtime - std::filesystem::file_time_type::clock::now() +
            std::chrono::system_clock::now());
        if (sctp < cutoff) {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

}  // namespace agent
