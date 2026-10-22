/**
 * @file mcp_config.h
 * @brief MCP server 配置解析（Issue #27）
 * @details 解析标准 mcp.json 格式（Claude Code / VS Code 兼容）：
 *          - 用户级：~/.workx/mcp.json
 *          - 项目级：<cwd>/.mcp.json（项目级优先合并）
 *          支持 stdio（command/args/env）与 http（url/headers）两类 server。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/utils/result_v2.h"

namespace agent::mcp {

/// @brief 单个 MCP server 配置
struct McpServerConfig {
    std::string name;

    // stdio 传输
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;

    // http 传输（type=http）
    std::string url;
    std::map<std::string, std::string> headers;
    /// 允许连接内网/回环地址（本地 MCP server 用；默认 false = 启用 SSRF 防护）
    bool allow_private = false;

    /// 是否为 http 传输（否则为 stdio）
    bool is_http() const { return !url.empty(); }
    /// 是否有效（stdio 需 command；http 需 url）
    bool valid() const { return is_http() ? !url.empty() : !command.empty(); }
};

/// @brief 加载 MCP server 配置
/// @param user_config_dir 用户配置目录（如 ~/.workx），读取 <dir>/mcp.json
/// @param cwd 项目目录，读取 <cwd>/.mcp.json（项目级优先合并，同名 server 覆盖用户级）
/// @return 合并后的 server 配置列表；文件不存在返回空列表（非错误）
ResultV2<std::vector<McpServerConfig>> load_mcp_configs(
    const std::filesystem::path& user_config_dir, const std::filesystem::path& cwd);

/// @brief 从 JSON 对象解析 server 配置（供测试与单文件加载复用）
/// @param json 形如 { "mcpServers": { "<name>": {...} } }
/// @return 解析出的 server 列表；无效条目跳过（不阻断整体）
std::vector<McpServerConfig> parse_mcp_config_json(const nlohmann::json& json);

}  // namespace agent::mcp
