/**
 * @file web_search_tool.h
 * @brief WebSearchTool — 网页搜索工具（Tavily + SearXNG 免 Key 兜底）
 * @details 查询 Tavily API（有 Key）或 SearXNG 实例（免 Key 兜底），
 *          输出 "[index] 标题 · URL\n摘要" 格式文本。
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <cstddef>
#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief WebSearchTool — 搜索网页
///
/// Provider 链（按配置 web.search.provider 选择）：
///   - tavily（默认）：POST https://api.tavily.com/search，需 API Key
///   - searxng：GET {web.search.searxng_url}/search?q=..&format=json，免 Key
/// 无 Tavily Key 或 Tavily 限流/失败时自动回退到 SearXNG。
/// 结果按 " [1] 标题 · URL\n    摘要文本" 格式文本化返回，便于 LLM 阅读。
/// API Key 来源（按优先级）：
///   1. AppConfig web.search.tavily_api_key（持久化于 ~/.workx/config.json）
///   2. 进程环境变量 TAVILY_API_KEY / WORKX_TAVILY_API_KEY
class WebSearchTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }

    // ---------- 纯函数（便于单测） ----------

    /// @brief 组装 Tavily 请求体，含 num_results 范围裁剪
    static nlohmann::json build_tavily_request(const std::string& api_key, const std::string& query,
                                               int num_results, const std::string& search_depth);

    /// @brief 把 Tavily JSON 响应 → 给模型看的文本
    /// @return 非空格式化文本；缺 results/空数组返回占位提示"未找到相关结果"
    static std::string parse_tavily_response(const nlohmann::json& response_json);

    /// @brief 把 SearXNG JSON 响应 → 给模型看的文本（results[] 结构与 Tavily 兼容）
    static std::string parse_searxng_response(const nlohmann::json& response_json);

    /// @brief 构造 SearXNG JSON 查询 URL（含 query URL 编码）
    static std::string build_searxng_url(const std::string& base_url, const std::string& query);

    /// @brief 校验 SearXNG 实例 URL 安全性（#25 P1-1/P2-2）
    /// @return true = 安全：仅 https scheme 且 host 未解析到内网/回环/链路本地
    static bool is_safe_searxng_url(const std::string& url);

    /// @brief 构造 Bing HTML 搜索 URL（免 Key 兜底）
    static std::string build_bing_url(const std::string& base_url, const std::string& query,
                                      int num_results);

    /// @brief 解析 Bing 搜索结果 HTML → 给模型看的文本
    static std::string parse_bing_response(const std::string& html);

    /// @brief num_results 合法范围 [kMinNumResults, kMaxNumResults]
    static constexpr int kDefaultNumResults = 8;
    static constexpr int kMinNumResults = 1;
    static constexpr int kMaxNumResults = 20;
    static constexpr int kDefaultTimeoutMs = 15000;
    static constexpr const char* kTavilyEndpoint = "https://api.tavily.com/search";
    /// 免 Key 兜底默认实例（可被 web.search.searxng_url 覆盖）
    static constexpr const char* kDefaultSearxngUrl = "https://searx.be";
    /// 免 Key 兜底默认 Bing 端点（公共可达，无需 Key）
    static constexpr const char* kBingEndpoint = "https://www.bing.com/search";

    /// @brief 权限检查（#25）：Bypass 放行；搜索词命中内网路径/敏感关键词时 AskUser 确认
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
