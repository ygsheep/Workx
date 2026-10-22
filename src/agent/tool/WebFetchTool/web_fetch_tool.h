/**
 * @file web_fetch_tool.h
 * @brief WebFetchTool — 网页抓取工具
 * @details 抓取 URL 并转为简化版 Markdown（P0 strip 实现，未来 MCP 搜索/抓取接入即可替换）
 * @version 1.0.1
 * @date 2026-08
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief WebFetchTool — 网页抓取工具
///
/// 抓取指定 URL 的网页内容并转为简化 Markdown：
/// - 默认剥离 <script>/<style> 噪音
/// - 标题 h1~h6 → #~######
/// - 链接 <a href> → [text](url)、图片 ![alt](src)
/// - 代码块 <pre><code> → ```...```；<code> → `...`
/// - 列表 <ul>/<ol> → - / 1.
/// - 最大字符数 max_chars 默认 20000，超出按首段截断
class WebFetchTool : public ITool {
   public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    bool is_read_only() const override { return true; }

    /// @brief HTML→Markdown strip（纯函数，可单测）
    /// @param html 原始 HTML（UTF-8，若为 GBK 需提前解码）
    /// @param max_chars 输出上限（包含"已截断"提示在内），0 表示不截断
    static std::string html_to_markdown(std::string html, std::size_t max_chars = kDefaultMaxChars);

    static constexpr std::size_t kDefaultMaxChars = 20000;

    /// @brief 权限检查（#25）：Bypass 放行；内网/非法协议/非法端口硬拦截；
    ///        白名单域名自动放行，其余域名 AskUser 确认
    PermissionResult check_permissions(const nlohmann::json& input,
                                       const ToolContext& ctx) const override;

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;
};

}  // namespace agent::tool
