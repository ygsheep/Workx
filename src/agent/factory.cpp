/**
 * @file factory.cpp
 * @brief Agent 层会话装配工厂实现（宿主无关，B1 复用装配）
 * @details 从 workx_app/factory.cpp 上提的宿主无关逻辑；依赖仅限 agent + core，
 *          不触碰 tui/app。workx 与 codex 两个宿主都调用本工厂。
 * @version 1.0.0
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // 防止 windows.h 定义 min/max 宏干扰 std::numeric_limits
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include "agent/factory.h"

#include "agent/api/backend_factory.h"
#include "agent/api/chat_types.h"
#include "agent/api/i_backend.h"
#include "agent/api/i_backend_admin.h"  // C-2：dynamic_cast 到 IBackendAdmin*
#include "agent/config/app_config.h"    // keys:: / default_config_path()
#include "agent/core/chat_session.h"
#include "agent/mcp/mcp_client_manager.h"
#include "agent/model/provider_preset.h"
#include "agent/prompt/memory.h"          // 项目记忆加载（CLAUDE.md / AGENT.md）
#include "agent/session/session_store.h"  // 项目会话恢复
#include "agent/tool/AgentTool/agent_tool.h"
#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/AskUser/AskUserTool.h"
#include "agent/tool/BriefTool/BriefTool.h"
#include "agent/tool/PlanMode/enter_plan_mode_tool.h"
#include "agent/tool/PlanMode/exit_plan_mode_v2_tool.h"
#include "agent/tool/Task/task_output_tool.h"
#include "agent/tool/Task/task_stop_tool.h"
#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/GlobTool/glob_tool.h"
#include "agent/tool/GrepTool/grep_tool.h"
#include "agent/tool/ListMcpResourcesTool/list_mcp_resources_tool.h"
#include "agent/tool/MCPTool/mcp_tool.h"
#include "agent/tool/PowerShellTool/powershell_tool.h"
#include "agent/tool/ReadMcpResourceTool/read_mcp_resource_tool.h"
#include "agent/tool/ShellTool/shell_detector.h"
#include "agent/tool/SkillTool/skill_tool.h"
#include "agent/tool/TodoWriteTool/todo_write_tool.h"
#include "agent/tool/WebFetchTool/web_fetch_tool.h"
#include "agent/tool/WebSearchTool/web_search_tool.h"
#include "agent/tool/TaskTools/task_create_tool.h"
#include "agent/tool/TaskTools/task_get_tool.h"
#include "agent/tool/TaskTools/task_update_tool.h"
#include "agent/tool/TaskTools/task_list_tool.h"
#include "agent/tool/registry.h"
#include "core/config/config_manager.h"
#include "core/utils/file_index.h"
#include "core/utils/uuid.h"  // 项目会话恢复：UUID 生成

namespace agent {

// ============================================================
// create_backend
// ============================================================

BackendCreateResult create_backend(IConfigManager& cfg, const ProviderPreset* preset,
                                   IEventBus& event_bus) {
    BackendCreateResult result;

    // URL: cfg(显式设置) > preset > ""
    if (cfg.has(keys::REMOTE_URL)) {
        result.remote_url = cfg.get_or<std::string>(keys::REMOTE_URL, "");
    } else if (preset && !preset->default_url.empty()) {
        result.remote_url = std::string(preset->default_url);
    }

    // Model: cfg(显式设置) > preset > ""
    if (cfg.has(keys::MODEL_NAME)) {
        result.model_name = cfg.get_or<std::string>(keys::MODEL_NAME, "");
    } else if (preset && !preset->default_model.empty()) {
        result.model_name = std::string(preset->default_model);
    }

    // 无 remote_url 时不创建 backend
    if (result.remote_url.empty()) {
        return result;
    }

    // 构建 BackendConfig
    BackendConfig backend_config;
    backend_config.type = BackendConfig::Type::Remote;
    backend_config.provider = preset ? preset->type : ProviderType::OpenAI;
    backend_config.base_url = result.remote_url;
    backend_config.model_name = result.model_name;
    backend_config.api_key = cfg.get_or<std::string>(keys::API_KEY, "");
    int default_timeout = preset && preset->timeout_ms > 0 ? preset->timeout_ms : 30000;
    backend_config.timeout_ms = cfg.get_or<int>(keys::TIMEOUT_MS, default_timeout);
    // DS_CACHE P2：reasoning_content 往返配置（默认 false，仅 DeepSeek-reasoner 等 thinking
    // 模型开启）
    backend_config.send_reasoning_content = cfg.get_or<bool>(keys::SEND_REASONING, false);

    // 创建后端（H-1：显式注入 event_bus 以保留 BackendStatusEvent 发布；
    //              M-1：不再回退 EventBus::instance()）
    auto backend = BackendFactory::create(backend_config, &event_bus);
    if (!backend) {
        return result;  // provider 保持 nullptr
    }

    // 初始化后端（V2-3：initialize 返回 ResultV2）
    auto init_result = backend->initialize(backend_config);
    if (init_result.is_err()) {
        return result;  // provider 保持 nullptr
    }
    result.provider = std::move(backend);
    return result;
}

// ============================================================
// create_backend_for_entry — 按供应商条目创建后端（/provider 热切换）
// ============================================================

BackendCreateResult create_backend_for_entry(IConfigManager& cfg, const ProviderConfigEntry& entry,
                                             IEventBus& event_bus) {
    BackendCreateResult result;

    // 预置默认（可能为空；自定义条目 id 不在预设表返回 nullptr）
    const ProviderPreset* preset = find_preset(entry.id);

    // URL：条目显式 base_url > preset 默认 > ""（旧实现只用全局 cfg 旧值，切换失败根因）
    std::string effective_url = entry.base_url;
    if (effective_url.empty() && preset && !preset->default_url.empty()) {
        effective_url = preset->default_url;
    }
    result.remote_url = effective_url;

    // Model：条目 model > cfg > preset 默认
    std::string effective_model = entry.model;
    if (effective_model.empty()) {
        effective_model = cfg.get_or<std::string>(keys::MODEL_NAME, "");
    }
    if (effective_model.empty() && preset && !preset->default_model.empty()) {
        effective_model = preset->default_model;
    }
    result.model_name = effective_model;

    // 无 URL 时不创建 backend（ProviderConfigEntry 存在但未配置 base_url）
    if (effective_url.empty()) {
        return result;
    }

    // API Key：条目 api_key > cfg 全局（切换前 apply_provider_switch 尚未写 cfg，旧值不可靠）
    std::string api_key = entry.api_key;
    if (api_key.empty()) {
        api_key = cfg.get_or<std::string>(keys::API_KEY, "");
    }

    BackendConfig backend_config;
    backend_config.type = BackendConfig::Type::Remote;
    backend_config.provider = preset ? preset->type : ProviderType::OpenAI;
    backend_config.base_url = effective_url;
    backend_config.model_name = effective_model;
    backend_config.api_key = api_key;
    int default_timeout = preset && preset->timeout_ms > 0 ? preset->timeout_ms : 30000;
    backend_config.timeout_ms = cfg.get_or<int>(keys::TIMEOUT_MS, default_timeout);
    backend_config.send_reasoning_content = cfg.get_or<bool>(keys::SEND_REASONING, false);

    auto backend = BackendFactory::create(backend_config, &event_bus);
    if (!backend) {
        return result;  // provider 保持 nullptr
    }
    auto init_result = backend->initialize(backend_config);
    if (init_result.is_err()) {
        return result;  // provider 保持 nullptr（鉴权/网络等失败）
    }
    result.provider = std::move(backend);
    return result;
}

SessionResult create_session(IConfigManager& cfg, const ProviderPreset* preset,
                             ITaskManager& task_manager, IEventBus& event_bus) {
    SessionResult result;

    // 复用 create_backend：URL/Model 解析 + 后端创建与初始化
    auto backend_result = create_backend(cfg, preset, event_bus);

    // 兜底：当前 provider 为自定义条目（无 preset 默认 URL，且顶层未显式配置
    // remote_url）时，create_backend 解析不出 URL → 启动无会话 → 之后 /provider
    // 热切换因 session==null 静默失效（面板关闭但配置/界面无任何变化）。
    // 此时按 cfg.PROVIDER 在 providers 列表中定位活动条目，以条目自身配置创建后端。
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

    result.remote_url = backend_result.remote_url;
    result.model_name = backend_result.model_name;

    // 无 remote_url 时不创建会话
    if (result.remote_url.empty() || !backend_result.provider) {
        return result;
    }

    // 构造 ChatSession（M-1：显式注入 task_manager / event_bus / cfg，不再用单例）
    // C-2：先构造 session，再从 session 暴露的 admin 接口获取 backend_admin
    //      （避免 std::move(backend) 之前赋值导致 ChatSession 构造抛异常时悬垂指针）
    // 项目会话恢复：生成 UUID 作为 session_id（替换硬编码 "default"）
    std::string session_id = core::util::generate_uuid();
    int default_retry_delay = preset && preset->retry_delay_ms > 0 ? preset->retry_delay_ms : 1000;
    result.session = std::make_unique<ChatSession>(std::move(backend_result.provider), task_manager,
                                                   event_bus, cfg, default_retry_delay, session_id);

    // #45：CLI --bypass-permissions 启动即全权模式（跳过文件/命令确认）
    if (cfg.get_or<bool>(keys::BYPASS_PERMISSIONS, false)) {
        result.session->set_permission_mode(tool::PermissionMode::BypassPermissions);
    }

    // C-2：session 构造成功后，backend 已由 session 持有。
    // 通过 ChatSession 暴露的 completion_provider() 获取 ICompletionProvider*，
    // 再 dynamic_cast 到 IBackendAdmin*（IBackend 同时继承两者）。
    // session 存活期间 backend_admin 始终有效；session 析构后禁止使用。
    if (auto* provider = result.session->completion_provider()) {
        result.backend_admin = dynamic_cast<IBackendAdmin*>(provider);
    }

    // 注册内置工具（B1：单一来源 register_builtin_tools，各宿主工具集同步）
    // #27：创建 MCP 连接管理器并后台连接（单个 server 失败不阻断会话；
    //      事件总线用于发布连接状态变化，UI 侧栏实时刷新）
    auto mcp_manager = std::make_shared<mcp::McpClientManager>(&event_bus);
    mcp_manager->load_and_connect(default_config_path().parent_path(),
                                  std::filesystem::current_path());
    result.mcp_manager = mcp_manager;  // #27 M4：暴露给 UI 层展示 server 状态
    auto tool_registry = std::make_shared<tool::ToolRegistry>();
    register_builtin_tools(*tool_registry, mcp_manager);
    result.session->set_tool_registry(tool_registry);
    // #56 方案 D：把父会话全局 MCP 管理器注入 ChatSession，AgentTool 子 Agent
    //              mcpServers 字符串引用从该管理器复用 client（引用复用不清理）。
    result.session->set_mcp_manager(mcp_manager);

    // 宿主接线：FileWriteTool 写文件后失效 TUI @ 补全索引（mark_dirty 仅原子置位）
    result.session->set_file_index_invalidator([] { global_file_index().mark_dirty(); });

    // 系统提示词
    const std::string user_prompt = cfg.get_or<std::string>(keys::SYSTEM_PROMPT, "");
    std::string sys_prompt = build_system_prompt(user_prompt, *tool_registry);
    if (!sys_prompt.empty()) {
        result.session->set_system_prompt(sys_prompt);
    }
    // 极简/标准模式切换时重建系统提示词（方案 A）：工具说明段随模式收窄/恢复。
    // 会话持有构建回调（捕获 user_prompt 与注册表），切换模式时由会话调用。
    result.session->set_system_prompt_builder(
        [user_prompt, tool_registry](tool::SessionMode mode) -> std::string {
            return build_system_prompt(user_prompt, *tool_registry, mode);
        });

    // DS_CACHE H-4：从 provider preset 或 cfg 注入上下文窗口到压缩器
    // 优先级：cfg.backend.context_length > preset.default_context_length > 0（压缩器内部 fallback
    // 1M）
    int32_t context_window = cfg.get_or<int>(keys::CONTEXT_LENGTH, 0);
    if (context_window <= 0 && preset && preset->default_context_length > 0) {
        context_window = preset->default_context_length;
    }
    if (context_window > 0) {
        result.session->set_compactor_context_window(context_window);
    }

    // DS_CACHE M-1：配置归档目录（compact 折叠前归档原消息，保证可追溯）
    // 派生自 session.save_path 的父目录 / "archive"，未配置 save_path 则跳过
    std::string save_path = cfg.get_or<std::string>(keys::SAVE_PATH, "");
    if (!save_path.empty()) {
        namespace fs = std::filesystem;
        fs::path archive_dir = fs::path(save_path).parent_path() / "archive";
        result.session->set_compactor_archive_dir(archive_dir.string());
    }

    // ============================================================
    // 项目会话恢复：配置懒创建 SessionStore（首条 user 消息时才创建文件）
    // ============================================================
    // 存储路径：<config_dir>/projects/<编码路径>/<session_id>.jsonl
    // factory 只传配置，不创建文件；ChatSession 在首条 user 消息时懒创建
    try {
        namespace fs = std::filesystem;
        fs::path config_dir = default_config_path().parent_path();
        std::string cwd = fs::current_path().string();
        fs::path project_dir = session::get_project_session_dir(config_dir, cwd);

        std::string git_branch;
        if (fs::exists(fs::current_path() / ".git")) {
            git_branch = "unknown";
        }
        result.session->configure_session_store(project_dir.string(), cwd, result.model_name,
                                                git_branch);
    } catch (const std::exception&) {
        // 配置失败不阻断会话启动，仅失去持久化能力
    }

    return result;
}

// ============================================================
// register_builtin_tools
// ============================================================

void register_builtin_tools(tool::ToolRegistry& registry,
                            std::shared_ptr<mcp::McpClientManager> mcp_manager) {
    registry.register_tool(std::make_shared<tool::FileReadTool>());
    registry.register_tool(std::make_shared<tool::FileWriteTool>());
    registry.register_tool(std::make_shared<tool::FileEditTool>());
    // SkillTool 的 CommandRegistry 晚于工具注册创建，先注册后注入
    registry.register_tool(std::make_shared<tool::SkillTool>(nullptr));
    registry.register_tool(std::make_shared<tool::BashTool>());
    registry.register_tool(std::make_shared<tool::GlobTool>());
    registry.register_tool(std::make_shared<tool::GrepTool>());
    registry.register_tool(std::make_shared<tool::AskUserTool>());
    // #56 方案 B：强制用户通信通道（开工/临门一脚确认）
    registry.register_tool(std::make_shared<tool::BriefTool>());
    // #28：计划模式工具（大型任务先规划后执行）
    registry.register_tool(std::make_shared<tool::EnterPlanModeTool>());
    registry.register_tool(std::make_shared<tool::ExitPlanModeV2Tool>());
    // #26：子 Agent 调度 + 后台任务查询/停止
    registry.register_tool(std::make_shared<tool::AgentTool>());
    registry.register_tool(std::make_shared<tool::TaskOutputTool>());
    registry.register_tool(std::make_shared<tool::TaskStopTool>());

    // #24：待办清单（TodoWrite 全量 + TaskV2 细粒度 CRUD）
    registry.register_tool(std::make_shared<tool::TodoWriteTool>());
    registry.register_tool(std::make_shared<tool::TaskCreateTool>());
    registry.register_tool(std::make_shared<tool::TaskGetTool>());
    registry.register_tool(std::make_shared<tool::TaskUpdateTool>());
    registry.register_tool(std::make_shared<tool::TaskListTool>());

    // #25：网页搜索 + 网页抓取（P0 简化版；P1 迁移到 MCP 搜索 Provider 以获得多引擎/去重/摘要能力）
    registry.register_tool(std::make_shared<tool::WebSearchTool>());
    registry.register_tool(std::make_shared<tool::WebFetchTool>());

    // #27：MCP 三件套（调用 MCP 工具 + 读取 MCP 资源）
    registry.register_tool(std::make_shared<tool::MCPTool>(mcp_manager));
    registry.register_tool(std::make_shared<tool::ListMcpResourcesTool>(mcp_manager));
    registry.register_tool(std::make_shared<tool::ReadMcpResourceTool>(mcp_manager));

    // Windows 平台额外注册 PowerShellTool（对齐 Claude Code 的条件注册策略）
    // BashTool（cmd.exe）和 PowerShellTool 并存，由模型根据任务特征自行选用
#ifdef _WIN32
    registry.register_tool(std::make_shared<tool::PowerShellTool>());
#endif
}

// ============================================================
// build_system_prompt
// ============================================================

namespace {

/// @brief 获取平台标识字符串（对齐 cc env.platform）
std::string get_platform_string() {
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

/// @brief 获取 OS 版本字符串（对齐 cc getUnameSR）
std::string get_os_version_string() {
#ifdef _WIN32
    // Windows: 用 RtlGetVersion 获取友好的版本名（避免 GetVersionEx 的 lie 模式）
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    // RtlGetVersion 不受 manifest 影响，返回真实版本
    typedef LONG(WINAPI * RtlGetVersionPtr)(OSVERSIONINFOEXW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto pRtlGetVersion =
            reinterpret_cast<RtlGetVersionPtr>(GetProcAddress(ntdll, "RtlGetVersion"));
        if (pRtlGetVersion && pRtlGetVersion(&osvi) == 0) {
            // 粗略判定 Windows 版本名
            const char* edition = "Windows";
            if (osvi.dwMajorVersion == 10) {
                edition = osvi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3) {
                edition = "Windows 8.1";
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2) {
                edition = "Windows 8";
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) {
                edition = "Windows 7";
            }
            return std::format("{} (build {})", edition, osvi.dwBuildNumber);
        }
    }
    return "Windows (unknown version)";
#else
    // POSIX: 用 uname -sr 等价信息
    struct utsname buf;
    if (uname(&buf) == 0) {
        return std::format("{} {}", buf.sysname, buf.release);
    }
    return "Unknown";
#endif
}

/// @brief 获取 shell 信息行（对齐 cc getShellInfoLine）
/// @details 通过 shell_detector 获取 BashTool 实际使用的 shell，
///          保证 env 段与 BashTool 的 prompt 完全一致
std::string get_shell_info_line() {
    const auto& sh = tool::shell_detect::detect();
    std::string bash_desc;
    if (sh.type == tool::shell_detect::ShellType::GitBash) {
        bash_desc = "Git Bash (" + sh.cmd + ")";
    } else if (sh.type == tool::shell_detect::ShellType::CmdExe) {
        bash_desc = "cmd.exe (degraded — Git Bash not found)";
    } else {
        // UnixSh
        bash_desc = sh.cmd;
    }

#ifdef _WIN32
    return bash_desc + " + PowerShell (powershell.exe)";
#else
    return bash_desc;
#endif
}

/// @brief 检测当前目录是否为 git 仓库
bool is_git_repo() {
    namespace fs = std::filesystem;
    fs::path cwd = fs::current_path();
    // 向上查找 .git 目录或文件
    for (fs::path p = cwd; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / ".git")) return true;
        if (p == p.parent_path()) break;
    }
    return false;
}

/// @brief 构建环境上下文段（对齐 cc computeEnvInfo 的 <env> 块）
std::string build_environment_context() {
    namespace fs = std::filesystem;
    std::string cwd = fs::current_path().string();
    std::string platform = get_platform_string();
    std::string os_version = get_os_version_string();
    std::string shell_line = get_shell_info_line();
    bool git_repo = is_git_repo();

    std::string env_block = std::format(
        "# Environment\n"
        "You are running on {}.\n"
        "- Platform: {}\n"
        "- Working directory: {}\n"
        "- Is directory a git repo: {}\n"
        "- Available shells: {}\n",
        os_version, platform, cwd, git_repo ? "Yes" : "No", shell_line);

    // Windows 平台补充 shell 选择指引
#ifdef _WIN32
    const auto& sh = tool::shell_detect::detect();
    if (sh.type == tool::shell_detect::ShellType::GitBash) {
        env_block +=
            "\n"
            "## Shell selection on Windows\n"
            "- **Bash tool** uses **Git Bash** — Unix commands (ls/grep/cat) are available.\n"
            "- **PowerShell tool** uses **powershell.exe** — for Windows-specific APIs "
            "(registry, WMI, .NET) and cmdlet pipelines.\n";
    } else {
        env_block +=
            "\n"
            "## Shell selection on Windows\n"
            "- **Bash tool** uses **cmd.exe** (degraded — Git Bash not found). "
            "Use Windows commands (dir, findstr, type, where). "
            "Unix commands like `ls`/`grep`/`cat` will FAIL.\n"
            "- **PowerShell tool** uses **powershell.exe** — supports aliases for "
            "ls/cat/cp/mv/rm. Prefer PowerShell for Unix-style commands on Windows.\n";
    }
#endif
    return env_block;
}

}  // anonymous namespace

std::string build_system_prompt(const std::string& user_prompt, const tool::ToolRegistry& registry,
                                tool::SessionMode mode) {
    std::string sys_prompt = user_prompt;

    // 注入环境上下文（<env> 段，对齐 Claude Code）
    sys_prompt += "\n\n";
    sys_prompt += build_environment_context();

    // 注入项目记忆（CLAUDE.md / AGENTS.md / AGENT.md，从 CWD 向上遍历）
    // 放在环境上下文之后、工具 prompt 之前，让项目约定优先级高于工具说明
    std::string project_memory =
        prompt::load_and_format_project_memory(std::filesystem::current_path());
    if (!project_memory.empty()) {
        sys_prompt += "\n\n";
        sys_prompt += project_memory;
    }

    // 拼接工具 prompt（极简模式仅白名单工具：Skill/Bash/Read/Write/Edit）
    for (const auto& t : registry.get_all_tools()) {
        if (mode == tool::SessionMode::Minimal && !tool::is_minimal_mode_tool(t->name())) {
            continue;
        }
        sys_prompt += "\n\n";
        sys_prompt += t->prompt();
    }

    // @file 引用说明：@path 只是路径引用，内容不注入；需要时用 Read 工具读取
    sys_prompt +=
        "\n\n"
        "用户消息中可能包含 @path 形式的文件引用（例如 @src/main.cpp），"
        "这只是文件路径，文件内容不会注入消息。如需读取该文件，"
        "请调用 Read 工具获取内容后再回答。";

    return sys_prompt;
}

}  // namespace agent
