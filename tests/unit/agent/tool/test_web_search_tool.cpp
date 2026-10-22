/**
 * @file test_web_search_tool.cpp
 * @brief WebSearchTool 单元测试（P0 简化版：Tavily 解析 + schema）
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

#include "agent/tool/WebSearchTool/web_search_tool.h"
#include "agent/tool/registry.h"
#include "agent/config/app_config.h"
#include "agent/factory.h"
#include "core/config/config_manager.h"

using namespace agent;
using namespace agent::tool;
using namespace Catch::Matchers;

// ============================================================================
// 1. 基础元数据 + schema
// ============================================================================

TEST_CASE("WebSearchTool 名称/描述/只读属性正确", "[web_search][meta]") {
    WebSearchTool t;
    REQUIRE(t.name() == "WebSearch");
    REQUIRE_FALSE(t.description().empty());
    REQUIRE(t.is_read_only() == true);
}

TEST_CASE("WebSearchTool schema 声明 query 必填，其他可选", "[web_search][schema]") {
    WebSearchTool t;
    auto s = t.input_schema();
    REQUIRE(s.at("type") == "object");
    REQUIRE(s.contains("properties"));
    REQUIRE(s.at("properties").contains("query"));
    REQUIRE(s.at("properties").at("query").at("type") == "string");
    const auto& req = s.at("required");
    REQUIRE(std::find(req.begin(), req.end(), "query") != req.end());

    // num_results 存在
    REQUIRE(s.at("properties").contains("num_results"));
    REQUIRE(s.at("properties").at("num_results").at("default").get<int>() == 8);
}

TEST_CASE("register_builtin_tools 注册后可找到 WebSearch 工具", "[web_search][registry]") {
    ToolRegistry registry;
    agent::register_builtin_tools(registry);
    auto tool = registry.find_by_name("WebSearch");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->is_read_only() == true);
}

// ============================================================================
// 2. Tavily JSON → 格式化结果文本的纯函数 parse_tavily_response
// ============================================================================

TEST_CASE("parse_tavily_response 正常格式化为 [index] title · url + snippet 文本",
          "[web_search][parse]") {
    nlohmann::json resp = {
        {"results", nlohmann::json::array(
                        {{{"title", "A"}, {"url", "https://a.com/x"}, {"content", "body A"}},
                         {{"title", "B"}, {"url", "https://b.com/y"}, {"content", "body B"}}})}};
    auto text = WebSearchTool::parse_tavily_response(resp);
    REQUIRE_THAT(text, ContainsSubstring("[1] A"));
    REQUIRE_THAT(text, ContainsSubstring("· https://a.com/x"));
    REQUIRE_THAT(text, ContainsSubstring("body A"));
    REQUIRE_THAT(text, ContainsSubstring("[2] B"));
    REQUIRE_THAT(text, ContainsSubstring("· https://b.com/y"));
    REQUIRE_THAT(text, ContainsSubstring("body B"));
}

TEST_CASE("parse_tavily_response answer 字段存在时置于顶端", "[web_search][parse]") {
    nlohmann::json resp = {
        {"answer", "Top answer summary"},
        {"results",
         nlohmann::json::array({{{"title", "A"}, {"url", "https://a.com"}, {"content", "b"}}})}};
    auto text = WebSearchTool::parse_tavily_response(resp);
    REQUIRE(text.find("Top answer summary") == 0);
    REQUIRE_THAT(text, ContainsSubstring("\n\n[1]"));
}

TEST_CASE("parse_tavily_response 缺 results 或空数组时返回空结果提示", "[web_search][parse]") {
    REQUIRE_THAT(WebSearchTool::parse_tavily_response({}), ContainsSubstring("未找到"));
    REQUIRE_THAT(WebSearchTool::parse_tavily_response({{"results", nlohmann::json::array()}}),
                 ContainsSubstring("未找到"));
}

TEST_CASE("parse_tavily_response 跳过 title/url 缺失的脏条目", "[web_search][parse]") {
    nlohmann::json resp = {
        {"results",
         nlohmann::json::array({{{"title", nullptr}, {"url", "https://a.com"}, {"content", "a"}},
                                {{"title", "B"}, {"url", "https://b.com"}, {"content", "b"}}})}};
    auto text = WebSearchTool::parse_tavily_response(resp);
    REQUIRE(text.find("[1]") != std::string::npos);  // 第一条被跳过，只留 B
    REQUIRE_THAT(text, ContainsSubstring("B"));
    REQUIRE_FALSE(ContainsSubstring("https://a.com").match(text));
}

// ============================================================================
// 3. Tavily 请求 body 构造：build_tavily_request 含必填字段
// ============================================================================

TEST_CASE("build_tavily_request 包含 api_key/query/max_results/search_depth",
          "[web_search][build]") {
    const std::string key = "tvly-test";
    auto body = WebSearchTool::build_tavily_request(key, "nlohmann install", 5, "basic");
    REQUIRE(body.at("api_key") == key);
    REQUIRE(body.at("query") == "nlohmann install");
    REQUIRE(body.at("max_results") == 5);
    REQUIRE(body.at("search_depth") == "basic");
    REQUIRE(body.at("include_answer") == false);
    REQUIRE(body.at("include_raw_content") == false);
}

TEST_CASE("build_tavily_request 覆盖默认 num_results 范围校验", "[web_search][build]") {
    auto body = WebSearchTool::build_tavily_request("k", "q", 0, "basic");
    REQUIRE(body.at("max_results") == 1);  // 下限裁剪
    body = WebSearchTool::build_tavily_request("k", "q", 999, "basic");
    REQUIRE(body.at("max_results") == 20);  // 上限裁剪
}

// ============================================================================
// 3. SearXNG（免 Key 兜底，#25）
// ============================================================================

TEST_CASE("build_searxng_url 构造 JSON 查询 URL 并编码 query", "[web_search][searxng]") {
    // 中文 + 空格 + 特殊字符 URL 编码
    auto u = WebSearchTool::build_searxng_url("https://searx.be", "C++ 教程");
    REQUIRE_THAT(u, Catch::Matchers::StartsWith("https://searx.be/search?q="));
    REQUIRE_THAT(u, Catch::Matchers::ContainsSubstring("format=json"));
    REQUIRE_THAT(u, Catch::Matchers::ContainsSubstring("safesearch=0"));
    // 中文被百分号编码（UTF-8）
    REQUIRE_THAT(u, Catch::Matchers::ContainsSubstring("%"));
    REQUIRE_FALSE(u.find(" ") != std::string::npos);
    // 尾部斜杠归一化
    auto u2 = WebSearchTool::build_searxng_url("https://searx.be/", "q");
    REQUIRE_THAT(u2, Catch::Matchers::StartsWith("https://searx.be/search?"));
}

TEST_CASE("parse_searxng_response 格式化 results 数组", "[web_search][searxng]") {
    nlohmann::json resp = {{"results", nlohmann::json::array({
                                           {{"title", "博客园文章"},
                                            {"url", "https://www.cnblogs.com/x/p/1.html"},
                                            {"content", "这是一篇 C++ 教程摘要"}},
                                           {{"title", "掘金文章"},
                                            {"url", "https://juejin.cn/post/123"},
                                            {"content", "前端工程化实践"}},
                                       })}};
    auto text = WebSearchTool::parse_searxng_response(resp);
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("[1] 博客园文章"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("https://www.cnblogs.com/x/p/1.html"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("[2] 掘金文章"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("前端工程化实践"));
}

TEST_CASE("parse_searxng_response 空结果返回占位提示", "[web_search][searxng]") {
    auto text = WebSearchTool::parse_searxng_response(nlohmann::json::object());
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("未找到"));
    auto text2 =
        WebSearchTool::parse_searxng_response(nlohmann::json{{"results", nlohmann::json::array()}});
    REQUIRE_THAT(text2, Catch::Matchers::ContainsSubstring("未找到"));
}

// ============================================================================
// 3.5 Bing（免 Key 兜底，#25）
// ============================================================================

TEST_CASE("build_bing_url 构造 HTML 搜索 URL 并编码 query", "[web_search][bing]") {
    auto u = WebSearchTool::build_bing_url("https://www.bing.com/search", "C++ 教程", 5);
    REQUIRE_THAT(u, Catch::Matchers::StartsWith("https://www.bing.com/search?q="));
    REQUIRE_THAT(u, Catch::Matchers::ContainsSubstring("count=5"));
    REQUIRE_THAT(u, Catch::Matchers::ContainsSubstring("mkt=zh-CN"));
    // 中文被百分号编码（UTF-8）
    REQUIRE_THAT(u, Catch::Matchers::ContainsSubstring("%"));
    REQUIRE_FALSE(u.find(" ") != std::string::npos);
    // 尾部斜杠归一化
    auto u2 = WebSearchTool::build_bing_url("https://www.bing.com/search/", "q", 3);
    REQUIRE_THAT(u2, Catch::Matchers::StartsWith("https://www.bing.com/search?q="));
    REQUIRE_THAT(u2, Catch::Matchers::ContainsSubstring("count=3"));
}

TEST_CASE("parse_bing_response 解析 b_algo 列表为 [index] title · url + 摘要",
          "[web_search][bing]") {
    const std::string html =
        "<html><body>"
        "<li class=\"b_algo\"><h2><a href=\"https://www.cnblogs.com/x/p/1.html\">"
        "博客园文章</a></h2><p>这是一篇 C++ 教程摘要</p></li>"
        "<li class=\"b_algo\"><h2><a href=\"https://juejin.cn/post/123\">"
        "掘金文章</a></h2><p>前端工程化实践</p></li>"
        "</body></html>";
    auto text = WebSearchTool::parse_bing_response(html);
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("[1] 博客园文章"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("https://www.cnblogs.com/x/p/1.html"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("这是一篇 C++ 教程摘要"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("[2] 掘金文章"));
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("前端工程化实践"));
}

TEST_CASE("parse_bing_response 解码 HTML 实体（数字/命名）", "[web_search][bing]") {
    const std::string html =
        "<li class=\"b_algo\"><h2><a href=\"https://a.com/x\">"
        "知&#236;识 &amp; 星球</a></h2><p>摘要&ensp;内容</p></li>";
    auto text = WebSearchTool::parse_bing_response(html);
    REQUIRE_THAT(text, Catch::Matchers::ContainsSubstring("知ì识 & 星球"));
    REQUIRE_FALSE(Catch::Matchers::ContainsSubstring("&amp;").match(text));
}

TEST_CASE("parse_bing_response 无结果页返回占位提示", "[web_search][bing]") {
    REQUIRE_THAT(WebSearchTool::parse_bing_response("<div class=\"b_no\">没有与此相关的结果</div>"),
                 Catch::Matchers::ContainsSubstring("未找到"));
    REQUIRE_THAT(WebSearchTool::parse_bing_response("<html>no b_algo here</html>"),
                 Catch::Matchers::ContainsSubstring("未找到"));
}

// ============================================================================
// 4. 权限检查（#25）
// ============================================================================

TEST_CASE("WebSearch check_permissions Bypass 模式全放行", "[web_search][perm]") {
    WebSearchTool t;
    ToolContext ctx;
    ctx.permission_mode = PermissionMode::BypassPermissions;
    auto r = t.check_permissions({{"query", "192.168.1.1 password"}}, ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("WebSearch check_permissions 普通搜索词默认放行", "[web_search][perm]") {
    WebSearchTool t;
    ToolContext ctx;
    auto r = t.check_permissions({{"query", "nlohmann json install"}}, ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("WebSearch check_permissions 敏感搜索词无确认通道时拒绝（fail-closed）",
          "[web_search][perm]") {
    WebSearchTool t;
    ToolContext ctx;  // 无 event_bus → ask_user_confirm 返回 false
    const char* kSensitive[] = {
        "192.168.1.1", "10.0.0.1",    "localhost", "127.0.0.1", "password",
        "api_key",     "private key", "密码",      "内网",
    };
    for (const char* q : kSensitive) {
        auto r = t.check_permissions({{"query", q}}, ctx);
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::PermissionDenied);
    }
}

// ============================================================================
// 5. 配置键（#25）：web.search.* 注册与默认值
// ============================================================================

TEST_CASE("register_config_defaults 注册 web.search 配置键", "[web_search][config]") {
    ConfigManager::instance().clear();
    register_config_defaults(ConfigManager::instance());
    // schema 已注册，默认值声明正确
    auto s = ConfigManager::instance().get_schema(keys::WEB_SEARCH_PROVIDER);
    REQUIRE(s.is_ok());
    REQUIRE(std::get<std::string>(s.value().default_value) == "tavily");
    auto s2 = ConfigManager::instance().get_schema(keys::WEB_SEARCH_TAVILY_KEY);
    REQUIRE(s2.is_ok());
    auto s3 = ConfigManager::instance().get_schema(keys::WEB_SEARCH_SEARXNG_URL);
    REQUIRE(s3.is_ok());
    REQUIRE(std::get<std::string>(s3.value().default_value) == "https://searx.be");
    // set/get 往返（工具侧用 get_or 显式兜底默认值）
    ConfigManager::instance().set(keys::WEB_SEARCH_TAVILY_KEY, std::string("tvly-test"));
    REQUIRE(ConfigManager::instance().get_or<std::string>(keys::WEB_SEARCH_TAVILY_KEY, "") ==
            "tvly-test");
    ConfigManager::instance().clear();
}

// ============================================================================
// 6. SearXNG URL 安全校验（#25 P1-1/P2-2）
// ============================================================================

TEST_CASE("is_safe_searxng_url 仅允许 https 且不得指向内网", "[web_search][searxng][ssrf]") {
    // 安全：公网 https（IP 字面量，不依赖 DNS）
    REQUIRE(WebSearchTool::is_safe_searxng_url("https://8.8.8.8"));
    // 非 https scheme
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("http://8.8.8.8"));
    // 非 http(s) scheme
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("file:///etc/passwd"));
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("gopher://8.8.8.8"));
    // 内网/回环/链路本地/元数据 IP
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("https://127.0.0.1"));
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("https://10.0.0.1"));
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("https://192.168.1.1"));
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("https://169.254.169.254"));
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("https://172.16.0.1"));
    // 空/畸形 URL
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url(""));
    REQUIRE_FALSE(WebSearchTool::is_safe_searxng_url("not-a-url"));
}

// ============================================================================
// 7. 敏感搜索词词边界匹配（#25 P3-3）
// ============================================================================

TEST_CASE("WebSearch 敏感词检测 词边界匹配避免误触", "[web_search][perm]") {
    WebSearchTool t;
    ToolContext ctx;  // 无 event_bus → ask_user_confirm 返回 false
    // 完整内网 IP 仍拒绝
    auto r1 = t.check_permissions({{"query", "192.168.1.1 配置"}}, ctx);
    REQUIRE(r1.is_err());
    auto r2 = t.check_permissions({{"query", "172.20.0.1 配置"}}, ctx);
    REQUIRE(r2.is_err());
    // 非完整 IP 片段不误触（旧逻辑 "172.2" 子串会误触版本号）
    auto r3 = t.check_permissions({{"query", "172.2 版本"}}, ctx);
    REQUIRE(r3.is_ok());
    // 公网 IP / 普通词不误触
    auto r4 = t.check_permissions({{"query", "8.8.8.8 dns"}}, ctx);
    REQUIRE(r4.is_ok());
    auto r5 = t.check_permissions({{"query", "top 10 devices"}}, ctx);
    REQUIRE(r5.is_ok());
}
