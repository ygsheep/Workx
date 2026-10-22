/**
 * @file mcp_client_manager.cpp
 * @brief MCP 连接管理实现
 * @details 从配置加载 server 列表，后台线程逐个连接；连接成功后预取工具清单缓存。
 *          单个 server 失败不阻断整体（记录状态与错误信息，其余正常连接）。
 *          每完成/失败一个即发布 McpStatusChangedEvent，供 UI 实时刷新侧栏。
 * @version 1.1.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_client_manager.h"

#include <algorithm>
#include <utility>

#include "agent/mcp/mcp_config.h"
#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"
#include "liblogger/logger.h"

namespace agent::mcp {

namespace {

/// @brief 清洗嵌入系统提示词的名称（P1-5 提示词注入防护）
/// @details 剥离控制字符（含换行/回车），防止恶意 server/工具名注入指令；
///          超长名截断，避免污染提示词。
std::string sanitize_name(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F) continue;  // 丢弃控制字符
        out += static_cast<char>(c);
    }
    constexpr size_t kMaxLen = 64;
    if (out.size() > kMaxLen) out.resize(kMaxLen);
    return out;
}

}  // anonymous namespace

McpClientManager::McpClientManager(IEventBus* event_bus) : m_event_bus(event_bus) {}

McpClientManager::~McpClientManager() {
    m_stop = true;
    if (m_worker.joinable()) m_worker.join();
}

void McpClientManager::load_and_connect(const std::filesystem::path& user_config_dir,
                                        const std::filesystem::path& cwd) {
    auto configs = load_mcp_configs(user_config_dir, cwd);
    if (configs.is_err()) {
        LOG_ERROR("[mcp] 加载 MCP 配置失败: {}", configs.error().message);
        return;
    }

    // 同步登记全部配置 server（状态=连接中），UI 立即可见
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_order.clear();
        m_status.clear();
        for (const auto& cfg : configs.value()) {
            m_order.push_back(cfg.name);
            McpServerStatus st;
            st.name = cfg.name;
            st.state = McpServerState::Connecting;
            m_status[cfg.name] = std::move(st);
        }
    }
    publish_status();

    // 后台连接：不阻塞 TUI 启动（npx 下载/HTTP 握手可能耗时数秒~数十秒）
    if (m_worker.joinable()) m_worker.join();  // 上次连接未结束则等待
    m_stop = false;
    m_worker = std::thread(
        [this, configs = std::move(configs.value())]() mutable { connect_all_async(configs); });
}

void McpClientManager::connect_all_async(const std::vector<McpServerConfig>& configs) {
    for (const auto& cfg : configs) {
        if (m_stop.load()) break;

        auto client = std::make_shared<McpClient>();
        auto result = client->connect(cfg);
        if (result.is_err()) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                auto& st = m_status[cfg.name];
                st.state = McpServerState::Failed;
                st.error = result.error().message;
            }
            LOG_WARN("[mcp] MCP server '{}' 连接失败: {}", cfg.name, result.error().message);
            publish_status();
            continue;
        }

        // 预取工具清单（失败不阻断，仅无工具列表）
        std::vector<std::string> tool_names;
        auto tools = client->list_tools();
        if (tools.is_ok()) {
            for (const auto& t : tools.value()) {
                tool_names.push_back(t.name);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clients[cfg.name] = client;
            m_tool_names[cfg.name] = tool_names;
            auto& st = m_status[cfg.name];
            st.protocol = client->protocol_version();
            st.tool_count = static_cast<int>(tool_names.size());
            st.state = McpServerState::Connected;
            st.error.clear();
        }
        LOG_INFO("[mcp] MCP server '{}' 已连接（协议 {}）", cfg.name, client->protocol_version());
        publish_status();
    }
}

void McpClientManager::publish_status() {
    if (!m_event_bus) return;
    std::vector<agent::McpServerStatusLite> lite;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        lite.reserve(m_order.size());
        for (const auto& name : m_order) {
            auto it = m_status.find(name);
            if (it == m_status.end()) continue;
            const auto& st = it->second;
            lite.push_back(agent::McpServerStatusLite{st.name, st.protocol, st.tool_count,
                                                      static_cast<int>(st.state), st.error});
        }
    }
    m_event_bus->publish_async(agent::McpStatusChangedEvent{std::move(lite)});
}

std::shared_ptr<McpClient> McpClientManager::get_client(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(name);
    return it != m_clients.end() ? it->second : nullptr;
}

std::shared_ptr<McpClient> McpClientManager::connect_one_off(const McpServerConfig& cfg,
                                                             int timeout_ms) {
    auto client = std::make_shared<McpClient>();
    auto result = client->connect(cfg, timeout_ms);
    if (result.is_err()) {
        LOG_WARN("[mcp] 临时 server '{}' 连接失败: {}", cfg.name, result.error().message);
        return nullptr;
    }
    return client;
}

void McpClientManager::dispose(const std::shared_ptr<McpClient>& client) {
    if (client) client->disconnect();
}

void McpClientManager::register_client(const std::string& name, std::shared_ptr<McpClient> client) {
    if (!client || name.empty()) return;
    // 预取工具名单（失败静默置空，不阻断注册）
    std::vector<std::string> tool_names;
    auto tools = client->list_tools();
    if (tools.is_ok()) {
        for (const auto& t : tools.value()) {
            tool_names.push_back(t.name);
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (std::find(m_order.begin(), m_order.end(), name) == m_order.end()) {
            m_order.push_back(name);
        }
        m_clients[name] = client;
        m_tool_names[name] = tool_names;
    }
}

std::vector<std::shared_ptr<McpClient>> McpClientManager::clients() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::shared_ptr<McpClient>> result;
    result.reserve(m_clients.size());
    for (const auto& [_, client] : m_clients) {
        result.push_back(client);
    }
    return result;
}

std::vector<std::string> McpClientManager::server_names() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    result.reserve(m_clients.size());
    for (const auto& [name, _] : m_clients) {
        result.push_back(name);
    }
    return result;
}

std::vector<McpServerStatus> McpClientManager::server_status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<McpServerStatus> result;
    result.reserve(m_order.size());
    for (const auto& name : m_order) {
        auto it = m_status.find(name);
        if (it != m_status.end()) result.push_back(it->second);
    }
    return result;
}

std::string McpClientManager::describe_servers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_clients.empty()) return "";
    std::string out;
    for (const auto& [name, _] : m_clients) {
        // P1-5：名称经清洗后嵌入提示词，防止提示词注入
        out += "- " + sanitize_name(name);
        auto it = m_tool_names.find(name);
        if (it != m_tool_names.end() && !it->second.empty()) {
            out += "（工具：";
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (i > 0) out += ", ";
                out += sanitize_name(it->second[i]);
            }
            out += "）";
        }
        out += "\n";
    }
    return out;
}

bool McpClientManager::has_tool(const std::string& server, const std::string& tool) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tool_names.find(server);
    if (it == m_tool_names.end()) return false;
    const auto& names = it->second;
    return std::find(names.begin(), names.end(), tool) != names.end();
}

bool McpClientManager::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_clients.empty();
}

}  // namespace agent::mcp
