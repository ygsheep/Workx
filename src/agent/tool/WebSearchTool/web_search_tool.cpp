/**
 * @file web_search_tool.cpp
 * @brief WebSearchTool 实现（P0：Tavily）
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/WebSearchTool/web_search_tool.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/api/remote/http_client.h"
#include "agent/api/remote/ssrf.h"
#include "agent/config/app_config.h"
#include "agent/tool/permission_ask.h"
#include "core/config/i_config_manager.h"
#include "core/utils/error.h"
#include "liblogger/logger.h"

namespace agent::tool {

namespace {

int clamp_num_results(int n) {
    if (n < WebSearchTool::kMinNumResults) return WebSearchTool::kMinNumResults;
    if (n > WebSearchTool::kMaxNumResults) return WebSearchTool::kMaxNumResults;
    return n;
}

std::string get_env(const char* name) {
#if defined(_WIN32)
    size_t needed = 0;
    char buf[256]{};
    if (::getenv_s(&needed, buf, sizeof(buf), name) == 0 && needed > 0) {
        return std::string(buf, needed > 0 ? needed - 1 : 0);
    }
    return {};
#else
    if (const char* v = std::getenv(name)) return v;
    return {};
#endif
}

std::string resolve_api_key(const ToolContext& ctx) {
    // 1. AppConfig web.search.tavily_api_key（load_from_env 已并入 TAVILY_API_KEY）
    if (ctx.config_manager_ptr) {
        auto& cfg = *ctx.config_manager_ptr;
        std::string k = cfg.get_or<std::string>(keys::WEB_SEARCH_TAVILY_KEY, "");
        if (!k.empty()) return k;
    }
    // 2. 进程环境变量兜底
    std::string k = get_env("TAVILY_API_KEY");
    if (!k.empty()) return k;
    return get_env("WORKX_TAVILY_API_KEY");
}

std::string resolve_provider(const ToolContext& ctx) {
    if (ctx.config_manager_ptr) {
        return ctx.config_manager_ptr->get_or<std::string>(keys::WEB_SEARCH_PROVIDER, "tavily");
    }
    return "tavily";
}

std::string resolve_searxng_url(const ToolContext& ctx) {
    std::string url;
    if (ctx.config_manager_ptr) {
        url = ctx.config_manager_ptr->get_or<std::string>(keys::WEB_SEARCH_SEARXNG_URL,
                                                          WebSearchTool::kDefaultSearxngUrl);
    }
    if (url.empty()) url = get_env("WORKX_SEARXNG_URL");
    if (url.empty()) url = WebSearchTool::kDefaultSearxngUrl;
    // #25 P1-1/P2-2：仅允许 https 且 host 不得指向内网，非法配置回退默认实例
    if (!WebSearchTool::is_safe_searxng_url(url)) {
        LOG_WARN(
            "[WebSearch] 配置的 SearXNG URL '{}' 非法（仅允许 https 且不得指向内网/回环），"
            "回退默认实例 {}",
            url, WebSearchTool::kDefaultSearxngUrl);
        url = WebSearchTool::kDefaultSearxngUrl;
    }
    return url;
}

// ============================================================
// URL 编码（SearXNG GET 查询参数）
// ============================================================
std::string url_encode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0xF];
        }
    }
    return out;
}

// ============================================================
// HTML 解析辅助（Bing 搜索结果页）
// ============================================================
std::string strip_tags(const std::string& s) {
    return std::regex_replace(s, std::regex(R"(<[^>]+>)"), "");
}

// 解码 HTML 实体：&#NNN; / &#xHH; / 常见命名实体（Bing 摘要含 &#236;、&ensp; 等）
std::string decode_html_entities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const std::size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 12) {
            out += s[i];
            continue;
        }
        const std::string ent = s.substr(i + 1, semi - i - 1);
        bool replaced = false;
        if (!ent.empty() && ent[0] == '#') {
            char* endp = nullptr;
            unsigned long cp = 0;
            if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                cp = std::strtoul(ent.c_str() + 2, &endp, 16);
            else
                cp = std::strtoul(ent.c_str() + 1, &endp, 10);
            if (endp && *endp == '\0' && cp > 0) {
                if (cp < 0x80)
                    out += static_cast<char>(cp);
                else if (cp < 0x800) {
                    out += static_cast<char>(0xC0 | (cp >> 6));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += static_cast<char>(0xE0 | (cp >> 12));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | (cp >> 18));
                    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    out += static_cast<char>(0x80 | (cp & 0x3F));
                }
                replaced = true;
            }
        } else {
            static const std::pair<const char*, const char*> kNamed[] = {
                {"amp", "&"},   {"lt", "<"},    {"gt", ">"},    {"quot", "\""},  {"apos", "'"},
                {"nbsp", " "},  {"ensp", " "},  {"emsp", " "},  {"middot", "·"}, {"hellip", "…"},
                {"mdash", "—"}, {"ndash", "–"}, {"times", "×"}, {"copy", "©"},   {"reg", "®"},
                {"trade", "™"}, {"laquo", "«"}, {"raquo", "»"}, {"lsquo", "‘"},  {"rsquo", "’"},
                {"ldquo", "“"}, {"rdquo", "”"}, {"bull", "•"},  {"sect", "§"},
            };
            for (const auto& [name, rep] : kNamed) {
                if (ent == name) {
                    out += rep;
                    replaced = true;
                    break;
                }
            }
        }
        i = replaced ? semi : i;
    }
    return out;
}

// ============================================================
// 敏感搜索词检测（#25）：命中内网路径/凭据类关键词时走 AskUser
// ============================================================
bool contains_sensitive_search(const std::string& query) {
    std::string lower = query;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 内网 IP / 主机名：词边界 + 完整网段匹配，避免 "top 10 devices" 之类误触
    // 覆盖 10/8、192.168/16、172.16/12、127.0.0.1、169.254/16、localhost
    static const std::regex kPrivateNetRe(
        R"((\b10\.\d+\.\d+\.\d+)|(\b192\.168\.\d+\.\d+)|(\b172\.(1[6-9]|2\d|3[01])\.\d+\.\d+)|(\b127\.0\.0\.1)|(\b169\.254\.\d+\.\d+)|(\blocalhost\b))",
        std::regex::icase);
    if (std::regex_search(lower, kPrivateNetRe)) return true;
    if (lower.find("内网") != std::string::npos) return true;
    // 凭据/敏感信息（子串匹配，词本身即敏感）
    static const char* kCredentialPatterns[] = {
        "password",    "passwd",  "secret",     "token", "api_key", "apikey",
        "private key", "ssh key", "credential", "密码",  "密钥",    "口令",
    };
    for (const char* p : kCredentialPatterns) {
        if (lower.find(p) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

const std::string& WebSearchTool::name() const {
    static const std::string n{"WebSearch"};
    return n;
}

const std::string& WebSearchTool::description() const {
    static const std::string d{
        "按关键词搜索互联网，返回标题、URL、摘要的结构化列表。"
        "用于获取时效性信息或验证资料出处。"};
    return d;
}

const std::string& WebSearchTool::prompt() const {
    static const std::string p{
        "Call WebSearch when the answer depends on recent events, product "
        "changes, or information not in your training data (release notes, "
        "latest docs, news). Keep queries concise English keywords and prefer "
        "num_results ≤ 8. Follow-up unclear items via WebFetch."};
    return p;
}

nlohmann::json WebSearchTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties",
         {
             {"query",
              {{"type", "string"},
               {"description", "搜索关键词（必填）。建议用英文或中英混合的关键词，不必写整句。"}}},
             {"num_results",
              {{"type", "integer"},
               {"minimum", kMinNumResults},
               {"maximum", kMaxNumResults},
               {"default", kDefaultNumResults},
               {"description", "返回条数（1-20，默认 8）"}}},
             {"search_depth",
              {{"type", "string"},
               {"enum", nlohmann::json::array({"basic", "advanced"})},
               {"default", "basic"},
               {"description", "深度：basic 快（默认），advanced 抓取首条详情更准但更慢/更贵。"}}},
         }},
        {"required", {"query"}},
        {"additionalProperties", false}};
}

nlohmann::json WebSearchTool::build_tavily_request(const std::string& api_key,
                                                   const std::string& query, int num_results,
                                                   const std::string& search_depth) {
    const auto depth = (search_depth == "advanced") ? "advanced" : "basic";
    return {{"api_key", api_key},      {"query", query},
            {"search_depth", depth},   {"max_results", clamp_num_results(num_results)},
            {"include_answer", false}, {"include_raw_content", false},
            {"include_images", false}, {"include_image_descriptions", false}};
}

std::string WebSearchTool::parse_tavily_response(const nlohmann::json& r) {
    std::ostringstream out;
    bool has_answer = false;
    if (r.contains("answer") && r.at("answer").is_string()) {
        const std::string& a = r.at("answer").get_ref<const std::string&>();
        if (!a.empty()) {
            out << a;
            has_answer = true;
        }
    }
    const nlohmann::json* results = nullptr;
    if (r.contains("results") && r.at("results").is_array()) results = &r.at("results");
    int idx = 0;
    if (!results || results->empty()) {
        if (has_answer) return out.str();
        return "未找到相关搜索结果。建议更换关键词、尝试英文关键词或缩小范围。";
    }
    if (has_answer) out << "\n\n";
    for (const auto& item : *results) {
        if (!item.is_object()) continue;
        // 脏条目跳过：缺 title / url 或为 null（Tavily 返回数据偶发不完整）
        auto get_str_field = [](const nlohmann::json& obj, const char* key) -> std::string {
            if (!obj.contains(key)) return {};
            const auto& v = obj.at(key);
            if (v.is_null()) return {};
            if (!v.is_string()) return {};
            return v.get<std::string>();
        };
        const std::string title = get_str_field(item, "title");
        const std::string url = get_str_field(item, "url");
        const std::string content = get_str_field(item, "content");
        if (title.empty() || url.empty()) continue;
        idx++;
        out << "[" << idx << "] " << title << " · " << url << "\n    ";
        // 摘要限制 600 字符/条，避免单条撑爆上下文
        std::string snip = content;
        constexpr std::size_t kSnippet = 600;
        if (snip.size() > kSnippet) {
            snip.resize(kSnippet);
            snip += "…";
        }
        // 摘要里的换行 → \n + 4 空格缩进
        for (std::size_t i = 0; i < snip.size(); ++i) {
            if (snip[i] == '\r') continue;
            if (snip[i] == '\n') {
                out << '\n' << "    ";
            } else {
                out << snip[i];
            }
        }
        out << "\n\n";
    }
    if (idx == 0) return "未找到相关搜索结果。建议更换关键词、尝试英文关键词或缩小范围。";
    return out.str();
}

std::string WebSearchTool::parse_searxng_response(const nlohmann::json& r) {
    // SearXNG JSON: {"results": [{"title","url","content"}], ...}
    // 与 Tavily 的 results[] 结构兼容，直接复用格式化逻辑
    return parse_tavily_response(r);
}

std::string WebSearchTool::build_searxng_url(const std::string& base_url,
                                             const std::string& query) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "/search?q=" + url_encode(query) + "&format=json&safesearch=0";
}

bool WebSearchTool::is_safe_searxng_url(const std::string& url) {
    const ParsedUrl parsed = HttpClient::parse_url(url);
    if (parsed.scheme.empty() || parsed.host.empty()) return false;
    if (parsed.scheme != "https") return false;
    if (host_resolves_to_private(parsed.host)) return false;
    return true;
}

std::string WebSearchTool::build_bing_url(const std::string& base_url, const std::string& query,
                                          int num_results) {
    std::string base = base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    return base + "?q=" + url_encode(query) +
           "&count=" + std::to_string(clamp_num_results(num_results)) +
           "&setlang=zh-hans&mkt=zh-CN";
}

std::string WebSearchTool::parse_bing_response(const std::string& html) {
    // 无结果页特征：Bing 用文案提示（b_no 类名会误匹配 #b_notificationContainer，不采用）
    if (html.find("没有与此相关的结果") != std::string::npos ||
        html.find("There are no results") != std::string::npos) {
        return "未找到相关搜索结果。建议更换关键词、尝试英文关键词或缩小范围。";
    }
    std::ostringstream out;
    int idx = 0;
    const std::regex algo_re(R"(<li\s+class="b_algo"[\s\S]*?</li>)", std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), algo_re);
    const auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        const std::string block = it->str();
        std::smatch m;
        std::string title, url;
        // 标题 + URL：<h2...><a href="..." ...>title</a></h2>
        // 用自定义原始串分隔符 _h2，避免模式内 ")" 提前终止 R"(...)"
        if (std::regex_search(
                block, m,
                std::regex(R"_h2(<h2[^>]*>\s*<a[^>]*href="([^"]+)"[^>]*>([\s\S]*?)</a>)_h2",
                           std::regex::icase))) {
            url = m[1].str();
            title = decode_html_entities(strip_tags(m[2].str()));
        }
        if (title.empty() || url.empty()) continue;
        // 摘要：<p ...>...</p>（Bing 摘要可能含 <strong> 等标签）
        std::string snippet;
        if (std::regex_search(block, m,
                              std::regex(R"(<p[^>]*>([\s\S]*?)</p>)", std::regex::icase))) {
            snippet = decode_html_entities(strip_tags(m[1].str()));
        }
        idx++;
        out << "[" << idx << "] " << title << " · " << url << "\n    ";
        constexpr std::size_t kSnippet = 600;
        if (snippet.size() > kSnippet) {
            snippet.resize(kSnippet);
            snippet += "…";
        }
        for (std::size_t i = 0; i < snippet.size(); ++i) {
            if (snippet[i] == '\r') continue;
            if (snippet[i] == '\n')
                out << '\n' << "    ";
            else
                out << snippet[i];
        }
        out << "\n\n";
    }
    if (idx == 0) return "未找到相关搜索结果。建议更换关键词、尝试英文关键词或缩小范围。";
    return out.str();
}

// ================= 权限检查 =================

PermissionResult WebSearchTool::check_permissions(const nlohmann::json& input,
                                                  const ToolContext& ctx) const {
    if (is_bypass_mode(ctx.permission_mode)) {
        return PermissionResult::ok();
    }
    if (!input.contains("query") || !input.at("query").is_string()) {
        return PermissionResult::ok();  // 参数错误由 validate/call 处理
    }
    const std::string query = input.at("query").get<std::string>();
    // 搜索本身只读安全；仅当关键词命中内网路径/凭据类敏感词时弹确认
    if (contains_sensitive_search(query)) {
        if (ask_user_confirm(
                ctx,
                std::format("WebSearch "
                            "搜索词可能涉及敏感信息，请确认：\n\n```\n{}\n```\n\n允许执行该搜索？",
                            query))) {
            return PermissionResult::ok();
        }
        return PermissionResult::err(Error::Code::PermissionDenied, "用户拒绝执行该搜索");
    }
    return PermissionResult::ok();
}

namespace {

// ============================================================
// Provider 实现（#25）：Tavily（需 Key）/ SearXNG（免 Key）/ Bing（免 Key 兜底）
// ============================================================

ResultV2<ToolResult> search_tavily(const std::string& key, const std::string& query,
                                   int num_results, const std::string& depth) {
    auto body = WebSearchTool::build_tavily_request(key, query, num_results, depth);
    HttpClient client;
    auto http = client.post_json(WebSearchTool::kTavilyEndpoint, {}, body,
                                 WebSearchTool::kDefaultTimeoutMs);
    if (!http.is_ok()) {
        return ResultV2<ToolResult>::err(Error::Code::NetworkDisconnected,
                                         "WebSearch(Tavily) 请求失败: " + http.error().message,
                                         "endpoint=" + std::string(WebSearchTool::kTavilyEndpoint) +
                                             "; context=" + http.error().context);
    }
    const HttpResponse& resp = http.value();
    if (resp.is_rate_limited()) {
        return ResultV2<ToolResult>::err(
            Error::Code::HttpRateLimited,
            "WebSearch(Tavily) 被限流 (HTTP 429)，稍后重试或升级计划。",
            "endpoint=" + std::string(WebSearchTool::kTavilyEndpoint));
    }
    if (!resp.is_success()) {
        // 避免把 API key 返回内容泄露到日志；body 里去掉 key
        nlohmann::json safe_body = body;
        if (safe_body.contains("api_key")) safe_body["api_key"] = "***";
        return ResultV2<ToolResult>::err(
            Error::Code::HttpError,
            "WebSearch(Tavily) 返回 HTTP " + std::to_string(resp.status_code) + ": " + resp.body,
            "request=" + safe_body.dump());
    }
    nlohmann::json jresp;
    try {
        jresp = nlohmann::json::parse(resp.body);
    } catch (const std::exception& e) {
        return ResultV2<ToolResult>::err(
            Error::Code::InternalError,
            std::string("WebSearch(Tavily) 响应 JSON 解析失败: ") + e.what(),
            "body_len=" + std::to_string(resp.body.size()));
    }
    const std::string text = WebSearchTool::parse_tavily_response(jresp);
    std::ostringstream head;
    head << "# WebSearch 结果: " << query << "\n";
    head << "Provider: Tavily (" << depth << ", max=" << clamp_num_results(num_results) << ")\n\n";
    head << "---\n\n";
    head << text;
    return ResultV2<ToolResult>::ok(ToolResult::ok(head.str()));
}

ResultV2<ToolResult> search_searxng(const std::string& base_url, const std::string& query,
                                    int num_results) {
    const std::string url = WebSearchTool::build_searxng_url(base_url, query);
    HttpClient client;
    // #25 P1-1：SearXNG 实例 URL 来自可写配置/环境变量，强制开启 SSRF 防护
    client.set_block_private_ips(true);
    auto http = client.get(url, {}, WebSearchTool::kDefaultTimeoutMs);
    if (!http.is_ok()) {
        return ResultV2<ToolResult>::err(Error::Code::NetworkDisconnected,
                                         "WebSearch(SearXNG) 请求失败: " + http.error().message,
                                         "url=" + url);
    }
    const HttpResponse& resp = http.value();
    if (resp.is_rate_limited()) {
        return ResultV2<ToolResult>::err(
            Error::Code::HttpRateLimited,
            "WebSearch(SearXNG) 被限流 (HTTP 429)，可在配置 web.search.searxng_url 换实例。",
            "url=" + url);
    }
    if (!resp.is_success()) {
        return ResultV2<ToolResult>::err(
            Error::Code::HttpError,
            "WebSearch(SearXNG) 返回 HTTP " + std::to_string(resp.status_code) + ": " + resp.body,
            "url=" + url);
    }
    nlohmann::json jresp;
    try {
        jresp = nlohmann::json::parse(resp.body);
    } catch (const std::exception& e) {
        return ResultV2<ToolResult>::err(
            Error::Code::InternalError,
            std::string("WebSearch(SearXNG) 响应 JSON 解析失败: ") + e.what(),
            "body_len=" + std::to_string(resp.body.size()));
    }
    const std::string text = WebSearchTool::parse_searxng_response(jresp);
    std::ostringstream head;
    head << "# WebSearch 结果: " << query << "\n";
    head << "Provider: SearXNG (" << base_url << ", max=" << clamp_num_results(num_results)
         << ")\n\n";
    head << "---\n\n";
    head << text;
    return ResultV2<ToolResult>::ok(ToolResult::ok(head.str()));
}

ResultV2<ToolResult> search_bing(const std::string& query, int num_results) {
    const std::string url =
        WebSearchTool::build_bing_url(WebSearchTool::kBingEndpoint, query, num_results);
    // 用标准 Chrome UA 模拟真实浏览器，避免被反爬拦截
    const std::vector<std::pair<std::string, std::string>> headers = {
        {"User-Agent",
         "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
         "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36"},
        {"Accept-Language", "zh-CN,zh;q=0.9,en;q=0.8"},
    };
    HttpClient client;
    auto http = client.get(url, headers, WebSearchTool::kDefaultTimeoutMs);
    if (!http.is_ok()) {
        return ResultV2<ToolResult>::err(Error::Code::NetworkDisconnected,
                                         "WebSearch(Bing) 请求失败: " + http.error().message,
                                         "url=" + url);
    }
    const HttpResponse& resp = http.value();
    if (resp.is_rate_limited()) {
        return ResultV2<ToolResult>::err(Error::Code::HttpRateLimited,
                                         "WebSearch(Bing) 被限流 (HTTP 429)，稍后重试。",
                                         "url=" + url);
    }
    if (!resp.is_success()) {
        return ResultV2<ToolResult>::err(
            Error::Code::HttpError,
            "WebSearch(Bing) 返回 HTTP " + std::to_string(resp.status_code) + ": " + resp.body,
            "url=" + url);
    }
    const std::string text = WebSearchTool::parse_bing_response(resp.body);
    std::ostringstream head;
    head << "# WebSearch 结果: " << query << "\n";
    head << "Provider: Bing (免 Key, max=" << clamp_num_results(num_results) << ")\n\n";
    head << "---\n\n";
    head << text;
    return ResultV2<ToolResult>::ok(ToolResult::ok(head.str()));
}

}  // namespace

ResultV2<ToolResult> WebSearchTool::call(const nlohmann::json& input,
                                         const ToolContext& ctx) const {
    if (!input.contains("query") || !input.at("query").is_string()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput,
                                         "WebSearch 需要字符串参数 query", input.dump());
    }
    const std::string query = input.at("query").get<std::string>();
    if (query.empty()) {
        return ResultV2<ToolResult>::err(Error::Code::InvalidInput, "WebSearch query 不能为空");
    }
    int num_results = kDefaultNumResults;
    if (input.contains("num_results")) {
        if (input.at("num_results").is_number_integer())
            num_results = input.at("num_results").get<int>();
        else if (input.at("num_results").is_string()) {
            try {
                num_results = std::stoi(input.at("num_results").get<std::string>());
            } catch (...) {
                num_results = kDefaultNumResults;
            }
        }
    }
    std::string depth = "basic";
    if (input.contains("search_depth") && input.at("search_depth").is_string())
        depth = input.at("search_depth").get<std::string>();

    // Provider 链（#25）：
    //   tavily（默认）+ 有 Key → Tavily；失败 → SearXNG → Bing（免 Key）
    //   searxng → SearXNG；失败 → Bing
    //   bing → 直接用 Bing（免 Key）
    //   其他未实现 provider → 回退 SearXNG → Bing
    const std::string provider = resolve_provider(ctx);
    const std::string key = resolve_api_key(ctx);
    const std::string searxng_url = resolve_searxng_url(ctx);

    auto try_searxng = [&]() -> ResultV2<ToolResult> {
        auto r = search_searxng(searxng_url, query, num_results);
        if (r.is_ok()) return r;
        LOG_WARN("[WebSearch] SearXNG 搜索失败，回退到 Bing: {}", r.error().message);
        return search_bing(query, num_results);
    };

    if (provider == "bing") {
        return search_bing(query, num_results);
    }
    if (provider == "searxng") {
        return try_searxng();
    }
    if (provider != "tavily") {
        LOG_WARN("[WebSearch] provider '{}' 尚未实现，回退到免 Key 的 SearXNG/Bing", provider);
        return try_searxng();
    }
    if (key.empty()) {
        LOG_WARN(
            "[WebSearch] 未配置 Tavily API Key（web.search.tavily_api_key 或 TAVILY_API_KEY），"
            "回退到免 Key 的 SearXNG/Bing");
        return try_searxng();
    }
    auto r = search_tavily(key, query, num_results, depth);
    if (r.is_ok()) return r;
    // Tavily 失败（限流/网络/HTTP 错误）→ 回退 SearXNG → Bing
    LOG_WARN("[WebSearch] Tavily 搜索失败，回退到 SearXNG/Bing: {}", r.error().message);
    return try_searxng();
}

}  // namespace agent::tool
