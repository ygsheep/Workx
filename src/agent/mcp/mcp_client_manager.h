/**
 * @file mcp_client_manager.h
 * @brief MCP 连接管理（Issue #27）
 * @details 管理所有已连接 MCP server 的 client：
 *          - 从配置加载并后台连接（会话启动时调用，不阻塞启动；失败不阻断）
 *          - 跟踪全部配置 server 的状态（连接中/已连接/失败+错误信息），供 UI 侧栏展示
 *          - 按 server 名获取 client
 *          - 缓存工具清单快照，供 MCPTool prompt 注入
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/mcp/mcp_client.h"
#include "agent/mcp/mcp_types.h"

namespace agent {
class IEventBus;
}

namespace agent::mcp {

/// @brief MCP server 连接状态
enum class McpServerState : uint8_t {
    Connecting = 0,  ///< 后台连接中
    Connected = 1,   ///< 已连接（工具清单已预取）
    Failed = 2,      ///< 连接失败（error 含原因）
};

/// @brief 全部配置 server 状态快照（#27 M4：供 UI 侧栏展示）
struct McpServerStatus {
    std::string name;      ///< server 名
    std::string protocol;  ///< 协商协议版本（"2026-07-28" / "2025-11-25"）
    int tool_count = 0;    ///< 已预取工具数
    McpServerState state = McpServerState::Connecting;  ///< 连接状态
    std::string error;                                  ///< 失败原因（state==Failed 时）
};

/// @brief MCP 连接管理器（生命周期 = 会话）
class McpClientManager {
   public:
    /// @param event_bus 事件总线（可选）：连接状态变化时异步发布
    ///                   McpStatusChangedEvent，供 UI 刷新侧栏
    explicit McpClientManager(IEventBus* event_bus = nullptr);
    ~McpClientManager();

    /// @brief 从配置加载并后台连接所有 server
    /// @param user_config_dir 用户配置目录（读取 <dir>/mcp.json）
    /// @param cwd 项目目录（读取 <cwd>/.mcp.json，项目级优先合并）
    /// @details 同步加载配置并登记全部 server（状态=连接中），随后后台线程逐个连接，
    ///          每完成/失败一个即发布 McpStatusChangedEvent；不阻塞调用方。
    void load_and_connect(const std::filesystem::path& user_config_dir,
                          const std::filesystem::path& cwd);

    /// @brief 按 server 名获取 client（不存在返回 nullptr）
    std::shared_ptr<McpClient> get_client(const std::string& name) const;

    /// @brief 同步连接一个独立临时 MCP server（#56 方案 D）
    /// @details 与 connect_all_async 不同：不注册进本 manager 的状态/客户端集合
    ///          （不得被 get_client/server_names 遮蔽），也不发布状态事件。
    ///          连接生命周期由调用方显式管理——持有返回的 client，用后调 dispose()。
    ///          内联 server 场景（AgentTool mcpServers inline 对象）由 AgentTool 调用。
    /// @param cfg 待连接的 server 配置
    /// @param timeout_ms 连接/协商超时
    /// @return 已连接 client；连接失败返回 nullptr（不抛异常）
    std::shared_ptr<McpClient> connect_one_off(const McpServerConfig& cfg, int timeout_ms = 15000);

    /// @brief 关闭临时 client（#56 方案 D，幂等）
    /// @details 等价 client->disconnect()；nullptr 或重复调用安全。仅用于关闭
    ///          connect_one_off 创建的临时 client，不影响 register_client 的复用 client。
    void dispose(const std::shared_ptr<McpClient>& client);

    /// @brief 将外部已连接 client 注册进本 manager（#56 方案 D）
    /// @details 子 Agent 作用域 manager 承接：@a inline —— connect_one_off 新连后注册
    ///          （需 dispose）；@a 引用 —— 从父 manager get_client 复用的实例注册
    ///          （不 dispose）。注册后该 server 经 get_client/describe_servers 对本
    ///          manager 可见，使 MCPTool（解析 ctx.mcp_manager_ptr）能调用。断开时将
    ///          在子作用域析构时自然释放，不影响父 manager 的生命周期。
    ///          同名重注册会覆盖旧条目（作用域隔离，父 manager 不受影响）。
    /// @param name server 名
    /// @param client 已连接 client（nullptr 忽略）
    void register_client(const std::string& name, std::shared_ptr<McpClient> client);

    /// @brief 所有已连接 client
    std::vector<std::shared_ptr<McpClient>> clients() const;

    /// @brief 已连接 server 名列表
    std::vector<std::string> server_names() const;

    /// @brief 全部配置 server 状态快照（含连接中/失败，按配置顺序）
    std::vector<McpServerStatus> server_status() const;

    /// @brief 已连接 server 的工具清单快照（供 MCPTool prompt 注入）
    /// @details 形如 "- github（工具：create_issue, list_pulls）\n- notion（...）"
    std::string describe_servers() const;

    /// @brief 指定 server 是否暴露指定工具（P1-6 工具存在性校验）
    /// @return false = server 不存在或未预取到该工具
    bool has_tool(const std::string& server, const std::string& tool) const;

    /// @brief 是否有任何已连接 server
    bool empty() const;

   private:
    /// @brief 后台连接线程体：逐个连接并更新状态、发布事件
    void connect_all_async(const std::vector<McpServerConfig>& configs);

    /// @brief 发布当前全量状态快照（McpStatusChangedEvent）
    void publish_status();

    mutable std::mutex m_mutex;
    /// 已连接 client（仅连接成功者）
    std::map<std::string, std::shared_ptr<McpClient>> m_clients;
    /// server 名 → 工具名列表（连接时预取缓存）
    std::map<std::string, std::vector<std::string>> m_tool_names;
    /// 全部配置 server 的状态（含连接中/失败）
    std::map<std::string, McpServerStatus> m_status;
    /// 配置顺序（server_status 按此返回，保证侧栏顺序稳定）
    std::vector<std::string> m_order;
    /// 后台连接线程（析构时置停止标志并 join）
    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    IEventBus* m_event_bus = nullptr;
};

}  // namespace agent::mcp
