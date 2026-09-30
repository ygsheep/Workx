/**
 * @file mcp_config.cpp
 * @brief MCP server 配置解析实现
 * @details 标准 mcp.json 格式解析：
 *          {
 *            "mcpServers": {
 *              "github": { "command": "npx", "args": [...], "env": {...} },
 *              "notion": { "type": "http", "url": "...", "headers": {...} }
 *            }
 *          }
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_config.h"

#include <algorithm>
#include <fstream>
#include <nlohmann/json.hpp>

#include "liblogger/logger.h"

namespace agent::mcp {

namespace {

/// @brief 校验 stdio command 安全性（P1-4 命令注入防护）
/// @details 拒绝控制字符与相对路径穿越（../、./）；允许裸命令名（PATH 解析，
///          如 npx/python）与绝对路径。参数由 CreateProcessW/execvp 直接传入，
///          不经 shell，故仅需拦截控制字符与路径穿越。
bool is_safe_stdio_command(const std::string& cmd) {
    if (cmd.empty()) return false;
    for (unsigned char c : cmd) {
        if (c < 0x20 || c == 0x7F) return false;  // 控制字符（含换行/制表符）
    }
    // 相对路径穿越（POSIX / Windows 分隔符）
    if (cmd.find("../") != std::string::npos || cmd.find("..\\") != std::string::npos ||
        cmd.starts_with("./") || cmd.starts_with(".\\")) {
        return false;
    }
    return true;
}

/// @brief 校验 stdio 参数安全性：仅拦截控制字符（换行等）
bool is_safe_stdio_arg(const std::string& arg) {
    for (unsigned char c : arg) {
        if (c < 0x20 || c == 0x7F) return false;
    }
    return true;
}

/// 解析单个 server 条目；无效条目返回 false
bool parse_one_server(const std::string& name, const nlohmann::json& obj, McpServerConfig& out) {
    if (!obj.is_object()) return false;

    out.name = name;

    // stdio：command + args + env
    if (obj.contains("command") && obj.at("command").is_string()) {
        out.command = obj.at("command").get<std::string>();
        if (!is_safe_stdio_command(out.command)) {
            return false;  // 命令含控制字符/路径穿越，拒绝该条目
        }
        if (obj.contains("args") && obj.at("args").is_array()) {
            for (const auto& a : obj.at("args")) {
                if (a.is_string()) {
                    const std::string arg = a.get<std::string>();
                    if (!is_safe_stdio_arg(arg)) return false;
                    out.args.push_back(arg);
                }
            }
        }
        if (obj.contains("env") && obj.at("env").is_object()) {
            for (auto it = obj.at("env").begin(); it != obj.at("env").end(); ++it) {
                if (it.value().is_string()) {
                    out.env[it.key()] = it.value().get<std::string>();
                }
            }
        }
        return out.valid();
    }

    // http：url（type=http 或省略 type 均可，兼容仅写 url 的配置）
    // 注意：command 优先（stdio）；仅含 url 时视为 http 传输
    if (obj.contains("url") && obj.at("url").is_string()) {
        out.url = obj.at("url").get<std::string>();
        if (obj.contains("headers") && obj.at("headers").is_object()) {
            for (auto it = obj.at("headers").begin(); it != obj.at("headers").end(); ++it) {
                if (it.value().is_string()) {
                    out.headers[it.key()] = it.value().get<std::string>();
                }
            }
        }
        out.allow_private = obj.value("allowPrivate", false);
        return out.valid();
    }

    return false;
}

/// 从单个文件加载 server 配置（文件不存在返回空，不视为错误）
ResultV2<std::vector<McpServerConfig>> load_from_file(const std::filesystem::path& path) {
    std::vector<McpServerConfig> result;
    if (path.empty() || !std::filesystem::exists(path)) {
        return ResultV2<std::vector<McpServerConfig>>::ok(std::move(result));
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return ResultV2<std::vector<McpServerConfig>>::err(
            Error::Code::ConfigParseFailed, "无法打开 MCP 配置文件: " + path.string(),
            path.string());
    }

    try {
        nlohmann::json root;
        ifs >> root;
        return ResultV2<std::vector<McpServerConfig>>::ok(parse_mcp_config_json(root));
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<std::vector<McpServerConfig>>::err(
            Error::Code::ConfigParseFailed, "MCP 配置文件 JSON 解析失败: " + std::string(e.what()),
            path.string());
    }
}

}  // anonymous namespace

std::vector<McpServerConfig> parse_mcp_config_json(const nlohmann::json& json) {
    std::vector<McpServerConfig> result;
    if (!json.is_object() || !json.contains("mcpServers")) return result;

    const auto& servers = json.at("mcpServers");
    if (!servers.is_object()) return result;

    for (auto it = servers.begin(); it != servers.end(); ++it) {
        McpServerConfig cfg;
        if (parse_one_server(it.key(), it.value(), cfg)) {
            result.push_back(std::move(cfg));
        }
    }
    return result;
}

ResultV2<std::vector<McpServerConfig>> load_mcp_configs(
    const std::filesystem::path& user_config_dir, const std::filesystem::path& cwd) {
    // 用户级
    auto user_result = load_from_file(user_config_dir / "mcp.json");
    if (user_result.is_err()) return user_result;
    std::vector<McpServerConfig> merged = std::move(user_result.value());

    // 项目级（同名 server 覆盖用户级）
    auto project_result = load_from_file(cwd / ".mcp.json");
    if (project_result.is_err()) return project_result;
    for (auto& cfg : project_result.value()) {
        // P2-4：项目级配置禁止启用 allowPrivate（防恶意仓库静默禁用 SSRF 防护）
        if (cfg.allow_private) {
            LOG_WARN(
                "[mcp] 项目级 .mcp.json 为 server '{}' 设置 allowPrivate=true，"
                "已忽略（仅用户级配置可启用）",
                cfg.name);
            cfg.allow_private = false;
        }
        auto it = std::find_if(merged.begin(), merged.end(),
                               [&](const auto& e) { return e.name == cfg.name; });
        if (it != merged.end()) {
            *it = std::move(cfg);
        } else {
            merged.push_back(std::move(cfg));
        }
    }
    return ResultV2<std::vector<McpServerConfig>>::ok(std::move(merged));
}

}  // namespace agent::mcp
