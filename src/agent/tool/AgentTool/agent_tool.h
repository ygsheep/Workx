/**
 * @file agent_tool.h
 * @brief AgentTool — 子 Agent 调度工具
 * @details 启动子 Agent 执行复杂任务
 * @version 1.3.1
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

#include "agent/api/chat_types.h"
#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/registry.h"
#include "agent/mcp/mcp_client_manager.h"
#include "agent/tool/itool.h"
#include "agent/tool/context.h"  // #54：PermissionMode 定义 + ITaskManager/IEventBus/ToolRegistry 等前向声明

namespace agent {
class Task;
}

namespace agent::tool {

/// @brief Agent 工具名常量（防递归排除 + #33 Coordinator 放行判断共用，避免硬编码漂移）
inline constexpr const char* kAgentToolName = "Agent";

/// @brief AgentTool — 子 Agent 调度工具
///
/// 启动子 Agent 处理独立子任务：
/// - 支持指定 prompt 和工具集
/// - 子 Agent 独立运行并返回结果
class AgentTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

    /// @brief 预加载 skill 到子 Agent 初始消息（#56 方案 C）
    /// @details 对每个 skill 名从 registry 解析出 PromptCommand，用 build_skill_full_text
    ///          取全文，生成一条 system 消息（每 skill 一条）。未知名/非技能条目静默跳过
    ///          （不阻断子 Agent 启动）。纯函数，供测试直接断言预加载结果。
    /// @param skills 待预加载的 skill 名列表
    /// @param registry 命令注册表（bundled + 磁盘技能）；nullptr 时返回空
    /// @param cctx 执行上下文（cwd/model/session），用于技能内展开；缺省用调用方注入
    /// @return 预加载的初始 system 消息（顺序 = skills 顺序，逐条过滤可用项）
    static std::vector<agent::ChatMessage> build_skill_preload_messages(
        const std::vector<std::string>& skills, const command::CommandRegistry* registry,
        const command::CommandContext& cctx = {});

    /// @brief 子 Agent MCP 作用域构建结果（#56 方案 D）
    /// @details 对 agent frontmatter 的 mcpServers 构建一个独立的子作用域 MCP 管理器：
    ///          - 字符串引用：从父管理器复用已 memoized client（owned_clients 不含，不 cleanup）
    ///          - inline 对象：connect_one_off 新建独立连接并入 owned_clients（子结束需 dispose）
    struct McpScopeBuildResult {
        std::shared_ptr<mcp::McpClientManager> scope;  ///< 子作用域管理器（可能为空，empty() 判定）
        std::vector<std::shared_ptr<mcp::McpClient>>
            owned_clients;  ///< inline 私有 client（需 dispose）
    };

    /// @brief 构建子 Agent 的 MCP 作用域管理器和需清理的 client（#56 方案 D）
    /// @details 纯函数，供 launch_sub_agent 与单元测试复用：解析 mcpServers 数组，
    ///          构造子作用域 manager（引用复用 / inline 新建），返回不可空的 scope
    ///          （empty()==true 表示无任何 server 可用）及需 dispose 的 inline client。
    ///          连接失败（如命令不存在）静默跳过对应 server，不抛异常（异常安全）。
    /// @param servers mcpServers 数组（元素为字符串引用或对象式 server 配置；null/空 → 空 scope）
    /// @param parent 父全局 MCP 管理器（字符串引用来源）；nullptr 时引用条目跳过
    /// @return 作用域构建结果
    static McpScopeBuildResult build_mcp_scope(const nlohmann::json& servers,
                                               mcp::McpClientManager* parent);
};

/// @brief 子 Agent 启动参数（聚合，避免长参数列表）
/// @details #54 起作为公共 API，供 AgentTool::call 与 Plan Mode V2 explore 并行启动复用。
struct SubAgentLaunchOptions {
    std::string task_id;  ///< 任务 id（AgentTool 生成 'a'+8 随机；Plan 用 "pa-N"）
    std::string prompt;   ///< 子 Agent 任务 prompt
    std::vector<std::string> tool_whitelist;  ///< 工具白名单（空 → 全部已注册工具）
    std::vector<std::string> skills;  ///< #56 方案 C：预加载到初始消息的 skill 名
    const command::CommandRegistry* command_registry =
        nullptr;  ///< #56 方案 C：按名取 skill 全文（非拥有）
    ICompletionProvider* provider = nullptr;  ///< LLM provider（宿主保证存活于会话周期）
    std::shared_ptr<ToolRegistry> sub_registry;  ///< 父会话工具注册表（构建子 Agent 独立工具集）
    IConfigManager* config_manager = nullptr;
    ITaskManager* task_manager = nullptr;
    IEventBus* event_bus = nullptr;
    std::string cwd;
    std::string session_id;  ///< #30：父会话 ID（注入子 Agent ToolContext.session_id，审计关联）
    tool::PermissionMode permission_mode = tool::PermissionMode::Default;
    std::shared_ptr<agent::hook::HookManager>
        hook_manager;            ///< #50：父循环 HookManager（SubagentStart/Stop 派发）
    nlohmann::json mcp_servers;  ///< #56 方案 D：mcpServers 数组（字符串引用 / inline 对象）
    mcp::McpClientManager* parent_mcp_manager =
        nullptr;  ///< #56 方案 D：父全局 MCP 管理器（复用来源，非拥有）
};

/// @brief 启动单个子 Agent 任务
/// @details v1.2.0：并行批量调度复用。为每个任务生成独立 task_id 并投递到线程池，
///          多个子 Agent 并发执行。返回已启动的 Task（供调用方等待/读输出）。
///          #54 起为公共 API，供 AgentTool::call 与 Plan explore runner 共用。
/// @param options 子 Agent 启动参数
/// @return 已启动的 Task
std::shared_ptr<agent::Task> launch_sub_agent(const SubAgentLaunchOptions& options);

}  // namespace agent::tool
