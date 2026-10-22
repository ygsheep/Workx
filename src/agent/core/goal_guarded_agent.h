/**
 * @file goal_guarded_agent.h
 * @brief GoalGuardedAgent — 目标导向 Agent（#31 / 里程碑 0.6.x）
 * @details 以"目标达成"为终止条件的 Agent 模式，包装现有 ReActLoop：
 *          每轮走一次 ReAct（Thought/Action/Observation），随后执行目标验证
 *          （check_goal）。达成 → 成功退出；未达成 → 注入"继续"提示并续一轮，
 *          直至达成或达到 max_attempts（失败退出、汇报当前状态）。
 *
 *          区别于普通 ReActLoop 的关键：LLM 主观的 FinalAnswer 不能作为
 *          成功信号——GoalGuarded 强制在每轮后跑 Verdict，未达成时即使 LLM
 *          已给出"我修好了"也会续轮，杜绝"自欺"（issue #31 场景 2）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/react_loop.h"    // IReActObserver / ReActResult / CacheAwareCompactor
#include "agent/core/goal_verdict.h"  // AgentGoal
#include "agent/api/i_completion_provider.h"
#include "agent/tool/registry.h"
#include "core/events/event_bus.h"

namespace agent {

// 前向声明（与 react_loop.h 一致，D-5：避免头文件强依赖）
class IConfigManager;
class ITaskManager;
namespace skill {
class TouchCollector;
}
// #56 方案 C：命令注册表（子 Agent skill 预加载来源）
namespace command {
class CommandRegistry;
}
namespace mcp {
class McpClientManager;
}  // namespace mcp

/// @brief GoalGuardedAgent 依赖注入（对齐 ReActLoop 构造参数，避免重复参数列表）
/// @details ReActLoop 每轮由本 Agent 新建（Config{max_iterations=1}），故所有
///          依赖需在本 Agent 生命周期内保持有效（含 compactor 跨轮持久）。
struct GoalAgentDeps {
    ICompletionProvider* provider = nullptr;
    std::shared_ptr<tool::ToolRegistry> registry;
    IConfigManager* config_manager = nullptr;
    ITaskManager* task_manager = nullptr;
    std::string cwd;
    /// 跨轮持久化的压缩器（nullptr 则 ReActLoop 内部新建，仅轮内有效）
    CacheAwareCompactor* external_compactor = nullptr;
    IEventBus* event_bus = nullptr;
    skill::TouchCollector* touch_collector = nullptr;
    std::function<void()> file_index_invalidator;
    std::string session_id;
    /// #28：会话级权限状态（Default/Plan/Bypass 三态 + 进 Plan 前原模式），
    /// 0.6.x：注入内部单步 ReActLoop，修复 GoalGuarded 路由丢失权限状态的缺口。
    tool::PermissionMode permission_mode{tool::PermissionMode::Default};
    tool::PermissionMode permission_mode_before_plan{tool::PermissionMode::Default};
    /// 会话工作模式（标准/计划/极简）：注入内部 ReActLoop，供极简模式白名单守卫
    tool::SessionMode session_mode{tool::SessionMode::Standard};
    /// 权限模式变更回调（空则不挂，宿主用来自查鼠标路径统一状态源）
    std::function<void(tool::PermissionMode /*mode*/, tool::PermissionMode /*before_plan*/,
                       bool /*in_plan*/)>
        permission_state_changed_cb;
    /// 0.6.x：查询追踪器（可选）。非空时每轮 Verdict 结果写入 tracker 的
    /// 当前记录（verdict_history），供 QueryEngine→queryTracking 调用链溯源。
    class QueryTracker* tracker = nullptr;
    /// 消息队列冲刷回调（可选，ChatSession 注入）：模型忙碌时前端入队的用户消息，
    /// 在 ReAct 工具轮边界合并为单条 user 消息注入当前循环的 messages。
    std::function<void(std::vector<ChatMessage>&)> queue_inject_cb;

    // #56 方案 C：命令注册表（bundled + 磁盘技能），子 Agent skill 预加载来源
    /// @brief 技能/命令注册表（可选，#56 方案 C）
    /// @details 注入到 ReActLoop → ToolContext.command_registry_ptr，供 AgentTool
    ///          按 skill 名预加载全文到子 Agent 初始消息。nullptr 时预加载跳过。
    std::shared_ptr<command::CommandRegistry> command_registry;

    // #56 方案 D：MCP 连接管理器（父会话全局），子 Agent 引用复用 mcp servers 来源
    /// @brief MCP 连接管理器（可选，#56 方案 D）
    /// @details 注入到 ReActLoop → ToolContext.mcp_manager_ptr，供 AgentTool 获取
    ///          全局连接（mcpServers 字符串引用复用）与子作用域隔离。nullptr 时
    ///          引用条目跳过、MCPTool 回退构造管理器。
    std::shared_ptr<mcp::McpClientManager> mcp_manager;
};

/// @brief 构造 GoalGuardedAgent 内部 ReActLoop 的工厂（供 QueryEngine 复用注入逻辑）
/// @details 注入途径与 ChatSession 构造 ReActLoop 完全一致；max_iterations=1 使每次
///          run() 只推进一轮 Thought+Action+Observation，由外层环控制续轮。
struct ReActLoopFactory {
    /// #33：构造一个已应用会话权限/事件的 ReActLoop（供角色 Agent 复用）。
    /// @param deps    依赖（permission 三态与变更回调会注入到 loop）
    /// @param registry 工具注册表（角色 Agent 可传入过滤后的子 registry）
    /// @param cfg     循环配置（默认全量 max_iterations；GoalGuarded 用 max_iterations=1）
    static std::unique_ptr<ReActLoop> make(const GoalAgentDeps& deps,
                                           std::shared_ptr<tool::ToolRegistry> registry,
                                           ReActLoop::Config cfg);

    /// 单步 ReActLoop（GoalGuarded 每轮一次），等价 make(deps, deps.registry, {1})
    static std::unique_ptr<ReActLoop> make_one_step(const GoalAgentDeps& deps);
};

/// @brief 目标导向 Agent
class WORKX_API GoalGuardedAgent {
   public:
    explicit GoalGuardedAgent(GoalAgentDeps deps);

    /// @brief 以目标达成（或超限）为终止条件运行
    /// @param messages 会话历史（跨轮累积，写入 assistant/tool/提示消息）
    /// @param system_prompt 系统提示词
    /// @param tools_schema 工具 schema
    /// @param should_cancel 外部取消信号
    /// @param goal 目标定义（None 时退化为一轮 ReAct）
    /// @param goal_spec agent.goal 原文（AgentVerdictEvent 透传/展示用，可空）
    /// @param observer 事件观察者（可选）
    /// @return 结果（含 goal_status 与最终 final_answer）
    ReActResult run(std::vector<ChatMessage>& messages, const std::string& system_prompt,
                    const nlohmann::json& tools_schema, const std::atomic<bool>& should_cancel,
                    const AgentGoal& goal, const std::string& goal_spec,
                    IReActObserver* observer = nullptr);

   private:
    GoalAgentDeps m_deps;
};

}  // namespace agent