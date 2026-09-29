/**
 * @file factory.h
 * @brief Agent 层会话装配工厂（宿主无关，B1 复用装配）
 * @details 从 workx_app/factory.cpp 上提的宿主无关装配逻辑：
 *          - create_session: Backend + ChatSession 组装
 *          - register_builtin_tools: 内置工具注册（单一来源，工具集同步）
 *          - build_system_prompt: 系统提示词拼接（env + 项目记忆 + 工具 prompt）
 *          依赖仅限 agent + core，不触碰 tui/app，供多个宿主（workx / codex）复用。
 * @version 1.0.0
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "agent/model/provider_config.h"

#include "core/config/i_config_manager.h"

#include "agent/tool/context.h"

namespace agent {
namespace tool {
class ToolRegistry;
}
}  // namespace agent
namespace agent::session {
class SessionStore;
}
namespace agent::mcp {
class McpClientManager;
}

namespace agent {

class ChatSession;
class IBackendAdmin;
class ICompletionProvider;
struct ProviderPreset;
class ITaskManager;
class IEventBus;

/// @brief 后端创建结果（工厂返回）
/// @details provider 为空表示配置不足（无 remote_url）或创建/初始化失败。
struct BackendCreateResult {
    std::unique_ptr<ICompletionProvider> provider;  ///< 创建的后端（可为空）
    std::string remote_url;                         ///< 解析后的 API URL
    std::string model_name;                         ///< 解析后的模型名
};

/// @brief 根据配置创建后端（不创建会话，供启动装配与运行时供应商热切换复用）
/// @details 与 create_session 前半段等价：
///          1. 从 cfg 读取 provider，查找 ProviderPreset
///          2. 解析 remote_url（cfg > preset > ""）和 model_name（cfg > preset > ""）
///          3. 若 remote_url 非空：BackendFactory::create → initialize
/// @param cfg 配置管理器
/// @param preset Provider 预设（nullptr 表示无预设）
/// @param event_bus 事件总线（BackendStatusEvent 发布用）
/// @return BackendCreateResult（provider 可能为 nullptr）
BackendCreateResult create_backend(IConfigManager& cfg, const ProviderPreset* preset,
                                   IEventBus& event_bus);

/// @brief 按供应商条目创建后端（/provider 热切换专用）
/// @details 以目标 ProviderConfigEntry 自身的连接配置为准，避免依赖切换前旧的
///          全局 cfg 值：URL = entry.base_url > preset 默认 > ""；model = entry.model
///          > cfg > preset；api_key = entry.api_key > cfg。这修复了自定义供应商
///          切换失败（旧实现只用 find_preset + 全局 cfg，自定义条目字段被忽略）。
/// @param cfg 配置管理器（其余字段如 timeout / reasoning 从 cfg 与 preset 解析）
/// @param entry 目标供应商条目
/// @param event_bus 事件总线（BackendStatusEvent 发布用）
/// @return BackendCreateResult（provider 可能为 nullptr）
BackendCreateResult create_backend_for_entry(IConfigManager& cfg, const ProviderConfigEntry& entry,
                                             IEventBus& event_bus);

/// @brief 会话创建结果（工厂返回）
/// @details H-8：新增 backend_admin 字段，UI 层通过它调用 list_models /
///          set_model_name 等管理接口，避免暴露完整 IBackend*。
///          #27 M4：新增 mcp_manager，UI 层读取已连接 MCP server 状态展示。
struct SessionResult {
    std::unique_ptr<ChatSession> session;  ///< 创建的会话（无 remote_url 时为 nullptr）
    std::string remote_url;                ///< 解析后的 API URL
    std::string model_name;                ///< 解析后的模型名
    IBackendAdmin* backend_admin =
        nullptr;  ///< H-8：后端管理句柄（非拥有，session 持有 backend 生命周期）
    std::shared_ptr<mcp::McpClientManager>
        mcp_manager;  ///< #27：MCP 连接管理器（非拥有，供 UI 查询状态）
};

/// @brief 创建 Backend + ChatSession
/// @details 执行流程：
///          1. 从 cfg 读取 provider，查找 ProviderPreset
///          2. 解析 remote_url（cfg > preset > ""）和 model_name（cfg > preset > ""）
///          3. 若 remote_url 非空：创建 Backend → initialize → 构造 ChatSession
///          4. 注册内置工具、拼接系统提示词
///          5. H-8：返回 IBackendAdmin* 给 UI 层调用管理接口
/// @param cfg 配置管理器
/// @param preset Provider 预设（nullptr 表示无预设）
/// @param task_manager 任务管理器（M-1：显式 DI，替代 TaskManager::instance()）
/// @param event_bus 事件总线（M-1：显式 DI，替代 EventBus::instance()）
/// @return SessionResult（session 可能为 nullptr）
SessionResult create_session(IConfigManager& cfg, const ProviderPreset* preset,
                             ITaskManager& task_manager, IEventBus& event_bus);

/// @brief 注册内置工具到 ToolRegistry（全量工具集，单一来源）
/// @details 注册 FileRead/FileWrite/FileEdit/SkillTool/Bash/Glob/Grep/AskUser/
///          PlanMode/AgentTool/TaskOutput/TaskStop；Windows 额外含 PowerShellTool。
///          #27：MCP 三件套（MCPTool/ListMcpResourcesTool/ReadMcpResourceTool）。
///          新增工具只需在此处维护，各宿主（workx / codex）自动同步。
/// @param registry 目标工具注册表
/// @param mcp_manager MCP 连接管理器（可为空，空则 MCP 工具返回"未连接"）
void register_builtin_tools(tool::ToolRegistry& registry,
                            std::shared_ptr<mcp::McpClientManager> mcp_manager = nullptr);

/// @brief 拼接系统提示词（含环境上下文、项目记忆和工具 prompt）
/// @param user_prompt 用户配置的系统提示词（可为空）
/// @param registry 已注册工具的工具注册表
/// @param mode 会话工作模式；极简模式（Minimal）下只拼白名单工具的 prompt
/// @return 拼接后的完整系统提示词
std::string build_system_prompt(const std::string& user_prompt, const tool::ToolRegistry& registry,
                                tool::SessionMode mode = tool::SessionMode::Standard);

}  // namespace agent
