/**
 * @file web_fetch_tool.cpp
 * @brief WebFetchTool 实现（P0：HttpClient GET + HTML→Markdown 第三方库）
 * @details HTML→Markdown 转换 **不自己造轮子**，优先链接 libcpp-html-to-md
 *          (FetchContent 从 GitHub 拉取)。若构建时库不可用，退化为基于
 *          std::regex 的极简 strip_tags 模式（不解析属性、换行压缩），
 *          保证功能可用。未来搜索和抓取迁移到 MCP 后可直接移除本层转换。
 * @version 1.0.2
 * @date 2026-08
 */

#include "agent/tool/WebFetchTool/web_fetch_tool.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <regex>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/api/remote/http_client.h"
#include "agent/api/remote/ssrf.h"
#include "agent/tool/permission_ask.h"
#include "core/utils/error.h"
#include "liblogger/logger.h"

#if WORKX_HAS_HTML2MD
#if WORKX_HTML2MD_VENDORED
// 已 vendor 的零依赖单头第三方库 kstenschke/html2md (MIT License)
// 仓库: https://github.com/kstenschke/html2md
// 用法: html2md::Converter::Convert(std::string*) —— in-place 不修改，返回新 string
#include <html2md/html2md.h>
#else
// FetchContent 路径下的 leventkaragol/libcpp-html-to-md (libxml2 后端, 质量更高)
#include <libcpp-html-to-md/markdown_converter.h>
#endif
#endif

namespace agent::tool {

namespace {

// ============================================================
// 噪音预处理：剥离 <script>/<style>/<head>/<!--comment--> 等内容
// （这部分不参与 MD 结构化转换，所有库/降级路径都在进入转换前执行）
// ============================================================

std::string erase_regex(const std::string& s, const std::regex& re) {
    return std::regex_replace(
        s, re, "", std::regex_constants::format_default | std::regex_constants::match_default);
}

std::string strip_noise_blocks(std::string html) {
    // 单行注释块
    html = erase_regex(html, std::regex(R"(<!--[\s\S]*?-->)", std::regex::optimize));
    // style/script/noscript/template/svg/iframe/head 整块删除
    static const char* kStripTags[] = {"script", "style",  "noscript", "template",
                                       "svg",    "iframe", "head"};
    for (const char* t : kStripTags) {
        std::string pat = std::string("<") + t + R"(\b[^>]*>[\s\S]*?</)" + t + R"(\s*>)";
        std::regex re(pat, std::regex::icase | std::regex::optimize);
        html = erase_regex(html, re);
        // 处理自闭合或无闭标签的残余：<script ...> 到文件结束
        std::regex open(std::string("<") + t + R"(\b[^>]*>)",
                        std::regex::icase | std::regex::optimize);
        std::smatch m;
        if (std::regex_search(html, m, open)) {
            html.erase(m.position(0), m.suffix().first - m.prefix().second -
                                          static_cast<std::ptrdiff_t>(m.position(0)));
            break;
        }
    }
    return html;
}

// ============================================================
// 降级：strip_tags 版极简转换（当第三方 html2md 不可用时）
// ============================================================
std::string strip_tags_plain(const std::string& s) {
    // 先把常见块级标签替换成双换行
    std::string out = std::regex_replace(
        s,
        std::regex(
            R"(</\s*(h[1-6]|p|div|section|article|aside|header|footer|nav|main|blockquote|figure|figcaption|pre|table|tr|ul|ol|li)\s*>)",
            std::regex::icase | std::regex::optimize),
        "\n\n");
    out = std::regex_replace(
        out, std::regex(R"(<\s*br\s*/?\s*>)", std::regex::icase | std::regex::optimize), "\n");
    // 去掉所有其他标签
    out = std::regex_replace(out, std::regex(R"(<[^>]+>)", std::regex::optimize), "");
    // entity 解码（常见子集）
    auto decode = [](std::string in) -> std::string {
        static const std::pair<const char*, const char*> kEnts[] = {
            {"&amp;", "&"},   {"&lt;", "<"},   {"&gt;", ">"},
            {"&quot;", "\""}, {"&apos;", "'"}, {"&nbsp;", " "}};
        for (auto [a, b] : kEnts) {
            size_t p = 0;
            while ((p = in.find(a, p)) != std::string::npos) {
                in.replace(p, std::strlen(a), b);
                p += std::strlen(b);
            }
        }
        return in;
    };
    out = decode(out);
    // 连续空白压缩 + 多空行压到 1 条
    std::istringstream iss(out);
    std::string line, res;
    bool first = true, prev_blank = true;
    while (std::getline(iss, line)) {
        // rtrim + 内部多空白折叠
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        std::string clean;
        bool sp = false;
        for (char c : line) {
            if (c == '\t') c = ' ';
            if (c == '\r') continue;
            if (c == ' ') {
                if (!sp) clean.push_back(' ');
                sp = true;
            } else {
                clean.push_back(c);
                sp = false;
            }
        }
        // ltrim
        size_t l = 0;
        while (l < clean.size() && clean[l] == ' ') ++l;
        if (l) clean.erase(0, l);

        if (clean.empty()) {
            if (!prev_blank) {
                res += '\n';
                prev_blank = true;
            }
            first = false;
            continue;
        }
        if (!first && !prev_blank) res += ' ';
        if (!first && prev_blank) res += '\n';
        res += clean;
        prev_blank = false;
        first = false;
    }
    // ltrim / rtrim
    size_t l = 0;
    while (l < res.size() && (res[l] == '\n' || res[l] == ' ')) ++l;
    if (l) res.erase(0, l);
    while (!res.empty() && (res.back() == '\n' || res.back() == ' ')) res.pop_back();
    return res;
}

std::string truncate_by_chars(std::string s, std::size_t max_chars) {
    if (max_chars == 0 || s.size() <= max_chars) return s;
    constexpr const char kTailHint[] =
        "\n\n[内容过长，前半已截断。可直接访问源 URL 获取完整内容。]";
    const std::size_t tail_len = sizeof(kTailHint) - 1;
    const std::size_t keep = (max_chars > tail_len) ? (max_chars - tail_len) : (max_chars / 2);
    s.resize(keep);
    s += kTailHint;
    return s;
}

// ============================================================
// SSRF 预检（#25）：协议 + 端口 + 主机解析
// ============================================================
// 发起请求前的第一层校验（快速失败）；连接层由 HttpClient 的
// opensocket 钩子兜底（覆盖 3xx 重定向后的最终目标地址）。
bool validate_fetch_url(const ParsedUrl& purl, std::string* reason) {
    if (purl.scheme != "http" && purl.scheme != "https") {
        if (reason) *reason = "仅允许 http/https 协议";
        return false;
    }
    if (purl.host.empty()) {
        if (reason) *reason = "缺少主机名";
        return false;
    }
    // 先做主机级 SSRF 判定（内网/回环/链路本地），再校验端口，
    // 保证内网地址即使带非标准端口也报 SSRF 而非端口错误
    if (host_resolves_to_private(purl.host)) {
        if (reason) *reason = "目标解析到内网/回环/链路本地地址，已拦截（SSRF）";
        return false;
    }
    // 端口白名单：仅 80/443（默认端口已由 parse_url 归一化）
    if (purl.port != "80" && purl.port != "443") {
        if (reason) *reason = "仅允许 80/443 端口";
        return false;
    }
    return true;
}

// ============================================================
// 域名白名单（#25）：常见可信站点自动放行，避免每次抓取都弹确认
// ============================================================
bool is_whitelisted_domain(const std::string& host) {
    static const char* kWhitelist[] = {
        "github.com",
        "raw.githubusercontent.com",
        "gist.github.com",
        "stackoverflow.com",
        "stackexchange.com",
        "superuser.com",
        "serverfault.com",
        "wikipedia.org",
        "wikimedia.org",
        "developer.mozilla.org",
        "learn.microsoft.com",
        "docs.microsoft.com",
        "docs.python.org",
        "nodejs.org",
        "react.dev",
        "vuejs.org",
        "kubernetes.io",
        "docker.com",
        "nginx.org",
        "apache.org",
        "cloudflare.com",
        "google.com",
        "microsoft.com",
        "apple.com",
        "amazon.com",
        "cnblogs.com",
        "csdn.net",
        "zhihu.com",
        "bilibili.com",
        "zsxq.com",
        "juejin.cn",
        "segmentfault.com",
        "jianshu.com",
        "example.com",
        "example.org",
        "example.net",
    };
    for (const char* d : kWhitelist) {
        if (host == d) return true;
        if (host.size() > std::strlen(d) && host.ends_with(std::string(".") + d)) return true;
    }
    return false;
}

}  // namespace

std::string WebFetchTool::html_to_markdown(std::string html, std::size_t max_chars) {
    if (html.empty()) return {};

    html = strip_noise_blocks(std::move(html));

    std::string md;
#if WORKX_HAS_HTML2MD
    try {
#if WORKX_HTML2MD_VENDORED
        md = html2md::Converter::Convert(&html);
#else
        md = MarkdownConverter::convert(html);
#endif
    } catch (const std::exception& e) {
        LOG_WARN("[WebFetch] 第三方 HTML→MD 抛异常，降级到 strip_tags: {}", e.what());
        md = strip_tags_plain(html);
    } catch (...) {
        LOG_WARN("[WebFetch] 第三方 HTML→MD 未知异常，降级到 strip_tags");
        md = strip_tags_plain(html);
    }
#else
    md = strip_tags_plain(html);
#endif

    return truncate_by_chars(std::move(md), max_chars);
}

// ================= 权限检查 =================

PermissionResult WebFetchTool::check_permissions(const nlohmann::json& input,
                                                 const ToolContext& ctx) const {
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    if (!input.contains("url") || !input.at("url").is_string()) {
        return PermissionResult::ok();  // 参数错误由 validate/call 处理
    }
    const std::string url = input.at("url").get<std::string>();
    auto purl = HttpClient::parse_url(url);
    std::string reason;
    // 内网/回环/链路本地/非法协议/非法端口 → 硬拦截（与 call() 双重防线）
    if (!validate_fetch_url(purl, &reason)) {
        return PermissionResult::err(Error::Code::PermissionDenied,
                                     "WebFetch 目标不安全: " + reason);
    }
    // 白名单域名自动放行；其余域名 AskUser 确认
    if (is_whitelisted_domain(purl.host)) {
        return PermissionResult::ok();
    }
    if (ask_user_confirm(
            ctx,
            std::format("WebFetch 需要抓取外部网页，请确认：\n\n```\n{}\n```\n\n允许访问该 URL？",
                        url))) {
        return PermissionResult::ok();
    }
    return PermissionResult::err(Error::Code::PermissionDenied, "用户拒绝访问该 URL");
}

// ================= 工具调用 =================

const std::string& WebFetchTool::name() const {
    static const std::string n{"WebFetch"};
    return n;
}

const std::string& WebFetchTool::description() const {
    static const std::string d{"抓取 HTTP(S) 网页并转为 Markdown，可用于获取时效性/未知内容。"};
    return d;
}

const std::string& WebFetchTool::prompt() const {
    static const std::string p{
        "Use WebFetch only when the question references a specific URL you "
        "cannot otherwise see (docs page, GitHub issue, blog post). Prefer "
        "sending 1 request with a few follow-up fetches over broad scraping; "
        "the response is converted to Markdown via a 3rd-party library and "
        "truncated at 20k chars. Do not fetch authentication-only, file://, "
        "or local-network addresses."};
    return p;
}

nlohmann::json WebFetchTool::input_schema() const {
    return {{"type", "object"},
            {"properties",
             {{"url", {{"type", "string"}, {"description", "要抓取的 HTTP/HTTPS URL（必填）"}}},
              {"prompt",
               {{"type", "string"},
                {"description",
                 "可选：指定需要提取的信息（如 'README 的 Install 章节'）。"
                 "当前 P0 版本作为提示写入结果头部；精准抽取留给未来 MCP 抓取层。"}}}}},
            {"required", {"url"}},
            {"additionalProperties", false}};
}

ResultV2<ToolResult> WebFetchTool::call(const nlohmann::json& input, const ToolContext& /*ctx*/
) const {
    if (!input.contains("url") || !input.at("url").is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput, "WebFetch 需要字符串参数 url",
                                         input.dump());
    }
    const std::string url = input.at("url").get<std::string>();
    // SSRF 预检（#25）：协议 + 端口 + 主机解析
    auto purl = HttpClient::parse_url(url);
    std::string ssrf_reason;
    if (!validate_fetch_url(purl, &ssrf_reason)) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "WebFetch 目标不安全: " + ssrf_reason, "url=" + url);
    }
    const std::string prompt = input.value("prompt", std::string{});

    HttpClient client;
    // 开启连接层 SSRF 钩子：拦截重定向后落到内网/回环/链路本地的最终目标
    client.set_block_private_ips(true);
    const std::vector<std::pair<std::string, std::string>> headers = {
        // 浏览器 UA：知乎/掘金等国内站点对非浏览器 UA 直接 403，用标准 Chrome UA 兼容
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8"}};
    auto http = client.get(url, headers, 15000);
    if (!http.is_ok()) {
        return ResultV2<ToolResult>::err(Error::Code::NetworkDisconnected,
                                         "WebFetch 抓取失败: " + http.error().message,
                                         "url=" + url + "; context=" + http.error().context);
    }
    const HttpResponse& resp = http.value();
    if (!resp.is_success()) {
        return ResultV2<ToolResult>::err(Error::Code::HttpError,
                                         "WebFetch 返回 HTTP " + std::to_string(resp.status_code),
                                         "url=" + url);
    }

    std::string md = html_to_markdown(resp.body, kDefaultMaxChars);
    std::ostringstream out;
    out << "# WebFetch 结果: " << url << "\n";
#if WORKX_HAS_HTML2MD
#if WORKX_HTML2MD_VENDORED
    out << "HTML→MD: 第三方库 kstenschke/html2md (vendored, zero-deps)\n";
#else
    out << "HTML→MD: 第三方库 libcpp-html-to-md (libxml2 后端, WORKX_HAS_HTML2MD=1)\n";
#endif
#else
    out << "HTML→MD: 极简 strip_tags 降级模式（构建时未检测到第三方 html2md 库）\n";
#endif
    out << "HTTP " << resp.status_code << " · body_len=" << resp.body.size() << "\n\n";
    if (!prompt.empty()) out << "提取提示: " << prompt << "\n\n";
    out << "---\n\n";
    out << md;
    return ResultV2<ToolResult>::ok(ToolResult::ok(out.str()));
}

}  // namespace agent::tool
