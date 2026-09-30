/**
 * @file app_config.h
 * @brief 应用配置键定义与配置加载
 * @details 配置键常量、默认值注册、环境变量/配置文件加载
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <filesystem>

namespace agent {

class IConfigManager;
class ConfigManager;

/// 配置键字符串常量
namespace keys {
// Terminal
constexpr const char* SIMPLE_IO = "terminal.simple_io";
constexpr const char* NO_COLOR = "terminal.no_color";
constexpr const char* VERBOSE = "terminal.verbose";
constexpr const char* PROMPT = "terminal.prompt";
// #45：--bypass-permissions 启动即全权模式（BypassPermissions，跳过文件/命令确认）
constexpr const char* BYPASS_PERMISSIONS = "terminal.bypass_permissions";

// Backend
constexpr const char* REMOTE_URL = "backend.remote_url";
constexpr const char* MODEL_NAME = "backend.model_name";
constexpr const char* API_KEY = "backend.api_key";
constexpr const char* PROVIDER = "backend.provider";
constexpr const char* PROVIDERS = "backend.providers";  // 多供应商配置列表（JSON 数组）
constexpr const char* TIMEOUT_MS = "backend.timeout_ms";
constexpr const char* CONTEXT_LENGTH =
    "backend.context_length";  // 模型上下文窗口（token），覆盖 preset 默认值
constexpr const char* SEND_REASONING =
    "backend.send_reasoning_content";  // DS_CACHE P2：是否把 reasoning_content
                                       // 往返发送给模型（DeepSeek-reasoner CoT 进前缀，默认 false）

// Retry
constexpr const char* RETRY_COUNT = "backend.retry_count";
constexpr const char* RETRY_DELAY_MS = "backend.retry_delay_ms";

// Session
constexpr const char* SYSTEM_PROMPT = "session.system_prompt";
constexpr const char* SAVE_PATH = "session.save_path";

// Agent
/// 当前 agent 名（空 = 无 agent 上下文）。声明了 frontmatter agent 字段的
/// skill 仅在该 agent 匹配时注入 system prompt / 触发 conditional 激活
constexpr const char* AGENT_ACTIVE = "agent.active";
/// 目标导向 Agent（agent.active=goal-guarded/verify）的目标声明。
/// 取值：tests_pass / build_clean / lint_zero / file_exists:<path> / cmd:<command>
/// #32 多模式：script:<command> / batch:cmd=<tmpl>&glob=<pattern>&concurrency=<n>
///             / watch:path=<dir>&cmd=<tmpl>&polls=<n>&interval=<ms>（agent.active 分别配合）
constexpr const char* AGENT_GOAL = "agent.goal";
/// ReAct 循环每轮基础预算（最大迭代轮数）。预算耗尽时若启用内部评审器
/// （agent.max_iterations + 停滞恢复），会评审"是否继续"并追加额外预算。
constexpr const char* AGENT_MAX_ITERATIONS = "agent.max_iterations";
/// #79：单次 AgentTool 调用允许派生的子 Agent 数量上限（tasks 数组长度上限）。
/// 超限时整批拒绝并返回结构化错误，由模型自行拆分任务或缩减并行规模。
/// 0 或负数表示不限制（不推荐）。默认 10。
constexpr const char* AGENT_SUB_AGENT_MAX_BATCH = "agent.sub_agent_max_batch";
/// #79：单次 run 累计允许派生的子 Agent 总数上限（跨多次 AgentTool 调用累计）。
/// 与 max_batch 形成两级护栏：单层规模 + 全程总量，防止成本无界增长。
/// 0 或负数表示不限制（不推荐）。默认 50。
constexpr const char* AGENT_SUB_AGENT_MAX_TOTAL = "agent.sub_agent_max_total";

// Plan Mode V2（#54：五阶段多 Agent 规划流程）
/// 进入 Plan 后是否自动触发 interview→explore→plan 全流程（false 仅保留原手动行为）
constexpr const char* PLAN_AUTO = "plan.auto";
/// 是否启用 Interview 阶段（进入 Plan 后先澄清需求/收集约束）
constexpr const char* PLAN_INTERVIEW_ENABLED = "plan.interview_enabled";
/// 并行 explore agent 数量（对应 cc getPlanModeV2ExploreAgentCount，默认 3）
constexpr const char* PLAN_EXPLORE_AGENT_COUNT = "plan.explore_agent_count";
/// explore 聚焦子域列表（逗号分隔；空 → 按 agent_count 生成通用探索 prompt）
constexpr const char* PLAN_EXPLORE_AREAS = "plan.explore_areas";

// Logging
constexpr const char* LOG_LEVEL = "logging.level";
constexpr const char* LOG_FILE = "logging.file";
/// 运行日志单文件超过该大小（MB）时轮转（0 = 不轮转），滚动为 workx.log.N
constexpr const char* LOG_MAX_SIZE_MB = "logging.max_size_mb";
/// 保留的运行日志轮转文件数（workx.log.1 .. workx.log.N）
constexpr const char* LOG_MAX_FILES = "logging.max_files";
/// 运行日志保留天数：启动时删除超过该天数的历史 workx_*.log（旧版时间戳命名，0 = 不清理）
constexpr const char* LOG_RETENTION_DAYS = "logging.retention_days";

// Tool — FileReadTool
constexpr const char* FILE_READ_MAX_SIZE = "tool.file_read.max_file_size_bytes";
constexpr const char* FILE_READ_MAX_LINES = "tool.file_read.max_lines_to_read";

// Tool — FileEditTool
/// 拒绝编辑的路径模式列表（换行分隔的 glob，如 "~/.ssh/**\n**/.env\n**/.git/**"）
constexpr const char* EDIT_DENY_PATTERNS = "tool.edit.deny_patterns";
/// 是否启用密钥扫描（写入内容包含疑似密钥时拒绝）
constexpr const char* EDIT_SCAN_SECRETS = "tool.edit.scan_secrets";

// Island（灵动岛 GUI IPC）
/// 是否启用 Island IPC 服务（默认 true；GUI 通过 named pipe/socket 发现本 TUI）
constexpr const char* ISLAND_ENABLED = "island.enabled";
/// 余额拉取汇率（CNY → USD 折算，DeepSeek 余额以 CNY 返回）
constexpr const char* ISLAND_USD_CNY_RATE = "island.usd_cny_rate";

// Audit（#37 审计日志）
constexpr const char* AUDIT_ENABLED = "audit.enabled";
constexpr const char* AUDIT_FILE = "audit.file";
constexpr const char* AUDIT_MAX_SIZE_MB = "audit.max_size_mb";
constexpr const char* AUDIT_RETENTION_DAYS = "audit.retention_days";

// Web（#25 WebSearchTool / WebFetchTool）
/// 搜索 Provider：tavily（默认）/ serper / searxng（P1 链式 fallback 预留）
constexpr const char* WEB_SEARCH_PROVIDER = "web.search.provider";
/// Tavily API Key（env TAVILY_API_KEY 覆盖）
constexpr const char* WEB_SEARCH_TAVILY_KEY = "web.search.tavily_api_key";
/// SearXNG 实例地址（免 Key 兜底；默认公共实例，可换自建）
constexpr const char* WEB_SEARCH_SEARXNG_URL = "web.search.searxng_url";

// Hooks（#50 通用 Hook 事件系统）
/// Hook 总开关（false 时全部禁用，QueryEngine 不构建 HookManager）
constexpr const char* HOOKS_ENABLED = "hooks.enabled";
/// hook 定义 JSON 数组（元素为 hook.HookDefinition 对象）
constexpr const char* HOOKS_DEFINITIONS = "hooks.definitions";
/// 全局默认 hook 超时（毫秒，单条未指定时使用）
constexpr const char* HOOKS_TIMEOUT_MS = "hooks.timeout_ms";
}  // namespace keys

/// @brief 注册所有配置项的结构化 Schema（类型/默认值/范围/枚举/环境变量映射）
/// @details v2.0.0 起使用 ConfigSchema 替代 ConfigMeta，由 ConfigManager 在
///          set_value() 时自动校验。环境变量绑定通过 Schema.env_var 声明。
/// @note M-2：接收 ConfigManager&（非 IConfigManager&），因 register_schema 是
///             ConfigManager 特有方法，不属于 IConfigManager 接口。配置初始化
///             本质是引导阶段，与业务组件的可测试性需求分离。
void register_config_defaults(ConfigManager& cfg);

/// @brief 从环境变量加载配置
/// @details 6 个标准环境变量由 ConfigManager::load_from_env() 按 Schema 加载：
///          WORKX_API_KEY/BASE_URL/MODEL/TIMEOUT/LOG_LEVEL/LOG_FILE。
///          WORKX_NO_COLOR 采用 presence-only 语义（兼容 no-color.org 规范）。
/// @note M-2：接收 ConfigManager&，原因同 register_config_defaults。
void load_from_env(ConfigManager& cfg);

/// @brief 从配置文件加载
/// @note M-2：接收 IConfigManager&，可注入 Mock 测试配置加载逻辑。
void load_from_config_file(IConfigManager& cfg, const std::filesystem::path& path);

/// @brief 默认配置文件路径（平台相关）
std::filesystem::path default_config_path();

/// @brief 默认日志文件路径（平台相关）
std::filesystem::path default_log_path();

/// @brief 日志目录（统一为配置目录下的 logs/）
std::filesystem::path log_dir();

/// @brief 清理过期运行日志：删除 logs/ 下超过 retention_days 天的 workx*.log（0 表示不清理）
void cleanup_expired_logs(int retention_days);

}  // namespace agent
