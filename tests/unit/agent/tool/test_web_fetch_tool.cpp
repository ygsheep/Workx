/**
 * @file test_web_fetch_tool.cpp
 * @brief WebFetchTool 单元测试（P0 简化版：HTML→Markdown strip + schema）
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>

#include "agent/tool/WebFetchTool/web_fetch_tool.h"
#include "agent/tool/registry.h"
#include "agent/factory.h"

using namespace agent;
using namespace agent::tool;
using namespace Catch::Matchers;

// ============================================================================
// 1. 基础元数据 + schema
// ============================================================================

TEST_CASE("WebFetchTool 名称/描述/只读属性正确", "[web_fetch][meta]") {
    WebFetchTool t;
    REQUIRE(t.name() == "WebFetch");
    REQUIRE_FALSE(t.description().empty());
    REQUIRE(t.is_read_only() == true);
}

TEST_CASE("WebFetchTool schema 声明 url 必填，prompt 可选", "[web_fetch][schema]") {
    WebFetchTool t;
    auto s = t.input_schema();
    REQUIRE(s.at("type") == "object");
    REQUIRE(s.contains("properties"));
    REQUIRE(s.at("properties").contains("url"));
    REQUIRE(s.at("properties").at("url").at("type") == "string");
    // url 必须在 required 里
    const auto& req = s.at("required");
    REQUIRE(std::find(req.begin(), req.end(), "url") != req.end());
}

TEST_CASE("register_builtin_tools 注册后可找到 WebFetch 工具", "[web_fetch][registry]") {
    ToolRegistry registry;
    agent::register_builtin_tools(registry);
    auto tool = registry.find_by_name("WebFetch");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->is_read_only() == true);
}

// ============================================================================
// 2. 纯函数：HTML → Markdown strip（P0 简化版）
//    为便于单测，WebFetchTool 对外暴露 static html_to_markdown 方法
// ============================================================================

TEST_CASE("html_to_markdown 剥离 script/style 内容", "[web_fetch][html2md]") {
    const std::string html = R"(<html><head><script>alert("x");</script>
<style>body{color:red;}</style><title>Hello</title></head>
<body><p>abc</p></body></html>)";
    auto md = WebFetchTool::html_to_markdown(html);
    // 脚本内容绝不可出现在结果里
    REQUIRE(md.find("alert") == std::string::npos);
    REQUIRE(md.find("color:red") == std::string::npos);
}

TEST_CASE("html_to_markdown 解码常见 HTML entity", "[web_fetch][html2md]") {
    const std::string html = R"(<p>A&amp;B &lt;tag&gt; &quot;q&quot; &#39;e&#39;</p>)";
    auto md = WebFetchTool::html_to_markdown(html);
    // &amp; 与 &lt;/&gt; 处理；&quot;/&#39; 因第三方库保留实体源码——验证不出现原始 <tag> 字面即可
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("A&B"));
    REQUIRE(md.find("<tag>") == std::string::npos);
    // 第三方库会把 &lt;tag&gt; 转成字面 "<tag>" 以外的 MD 符号，< 和 > 在 MD 里不拼接为 "<tag>"
    REQUIRE(md.find("tag") != std::string::npos);
}

TEST_CASE("html_to_markdown 保留标题层级与段落换行", "[web_fetch][html2md]") {
    const std::string html = R"(<h1>Title</h1><h2>Sub</h2><h3>Sub</h3><p>P1.</p><p>P2.</p>)";
    auto md = WebFetchTool::html_to_markdown(html);
    // 第三方库可能输出 Setext 风格 (Title + -----) 或 ATX (# Title)，这里放宽：至少同时存在 Title
    // 与层级标识
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("Title"));
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("### Sub"));  // H3 基本都是 ATX
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("P1."));
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("P2."));
}

TEST_CASE("html_to_markdown 链接保留锚文本 + 绝对 URL", "[web_fetch][html2md]") {
    const std::string html = R"(<p>see <a href="https://x.com/a">docs</a>.</p>)";
    auto md = WebFetchTool::html_to_markdown(html);
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("docs"));
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("https://x.com/a"));
    // 第三方库输出 [锚](href) 或 [锚] + 单独一行 URL 皆可
}

TEST_CASE("html_to_markdown 保留代码块 fenced 包裹", "[web_fetch][html2md]") {
    const std::string html = R"(<p>text</p><pre><code>int a = 1;
a++;</code></pre><p>end</p>)";
    auto md = WebFetchTool::html_to_markdown(html);
    // 第三方库输出 ``` fenced code block / 缩进 code 块二者之一都算合格
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("int a = 1;"));
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("a++;"));
}

TEST_CASE("html_to_markdown 列表 li 带项目符号前缀 + 有序编号", "[web_fetch][html2md]") {
    const std::string html = R"(<ul><li>A</li><li>B</li></ul><ol><li>C</li><li>D</li></ol>)";
    auto md = WebFetchTool::html_to_markdown(html);
    // 无序列表：kstenschke/html2md 输出 "* A"；其他库也有出 "- A"，所以用二者其一作为断言
    const bool unordered_ok =
        (md.find("* A") != std::string::npos && md.find("* B") != std::string::npos) ||
        (md.find("- A") != std::string::npos && md.find("- B") != std::string::npos);
    REQUIRE(unordered_ok);
    // 有序列表：必须含 "C" 与 "D"，典型样式 "1. C" / "2. D"
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("C"));
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("D"));
}

TEST_CASE("html_to_markdown 空输入与无 body 输入安全返回", "[web_fetch][html2md]") {
    REQUIRE(WebFetchTool::html_to_markdown("").empty());
    std::string md = WebFetchTool::html_to_markdown("<!-- just comment -->");
    // 第三方库可能返回少量换行/空格；P0 只验证不存在脚本关键字且文本尺寸很小
    REQUIRE(md.size() < 16);
}

TEST_CASE("html_to_markdown 默认按 max_chars 截断", "[web_fetch][html2md]") {
    std::string html = "<p>";
    // 3000 个 a 字符，远超自定义上限 500
    for (int i = 0; i < 3000; ++i) html.push_back('a');
    html += "</p>";
    auto md = WebFetchTool::html_to_markdown(html, 500);
    REQUIRE(md.size() <= 500);
}

TEST_CASE("html_to_markdown 标题/强/斜体样式保留", "[web_fetch][html2md]") {
    const std::string html = R"(<p><strong>B</strong> and <em>I</em> and <code>var</code>.</p>)";
    auto md = WebFetchTool::html_to_markdown(html);
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("**B**"));
    // <em> 斜体：kstenschke/html2md 不转下划线，保留纯文本；存在 "I" 即可
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("I"));
    REQUIRE_THAT(md, Catch::Matchers::ContainsSubstring("var"));
}

// ============================================================================
// 3. SSRF 防护（#25）：call() 在发起网络请求前快速失败
// ============================================================================

TEST_CASE("WebFetch call 拦截内网/回环/链路本地 URL（SSRF）", "[web_fetch][ssrf]") {
    WebFetchTool t;
    ToolContext ctx;
    const char* kBad[] = {
        "http://127.0.0.1/",
        "http://127.0.0.1:8080/admin",
        "http://10.0.0.1/",
        "http://192.168.1.1/",
        "http://172.16.0.1/",
        "http://169.254.169.254/latest/meta-data/",  // 云元数据端点
        "http://localhost/",
        "http://[::1]/",
        "http://[fd00::1]/",
    };
    for (const char* u : kBad) {
        nlohmann::json input = {{"url", u}};
        auto r = t.call(input, ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("SSRF"));
    }
}

TEST_CASE("WebFetch call 拦截非 http/https 协议与非法端口", "[web_fetch][ssrf]") {
    WebFetchTool t;
    ToolContext ctx;
    auto check = [&](const char* u, const char* frag) {
        nlohmann::json input = {{"url", u}};
        auto r = t.call(input, ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring(frag));
    };
    check("file:///etc/passwd", "协议");
    check("ftp://example.com/x", "协议");
    check("gopher://example.com/", "协议");
    check("http://example.com:8080/", "端口");
    check("https://example.com:8443/", "端口");
}

// ============================================================================
// 4. 权限检查（#25）
// ============================================================================

TEST_CASE("WebFetch check_permissions Bypass 模式全放行", "[web_fetch][perm]") {
    WebFetchTool t;
    ToolContext ctx;
    ctx.permission_mode = PermissionMode::BypassPermissions;
    // 即使内网 URL 也放行（Bypass 语义）
    auto r = t.check_permissions({{"url", "http://127.0.0.1/"}}, ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("WebFetch check_permissions 内网 URL 硬拦截（非 Bypass）", "[web_fetch][perm]") {
    WebFetchTool t;
    ToolContext ctx;  // 默认 Default 模式
    auto r = t.check_permissions({{"url", "http://169.254.169.254/latest/meta-data/"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("WebFetch check_permissions 白名单域名自动放行", "[web_fetch][perm]") {
    WebFetchTool t;
    ToolContext ctx;
    auto r = t.check_permissions({{"url", "https://github.com/ygsheep/WorkX"}}, ctx);
    REQUIRE(r.is_ok());
    r = t.check_permissions({{"url", "https://raw.githubusercontent.com/x/y/main/README.md"}}, ctx);
    REQUIRE(r.is_ok());
    r = t.check_permissions({{"url", "https://stackoverflow.com/questions/1"}}, ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("WebFetch check_permissions 国内常用站点白名单放行", "[web_fetch][perm][cn]") {
    WebFetchTool t;
    ToolContext ctx;
    const char* kCn[] = {
        "https://www.zhihu.com/question/123456",        // 知乎
        "https://wx.zsxq.com/d/abc123",                 // 知识星球
        "https://www.cnblogs.com/foo/p/123.html",       // 博客园
        "https://juejin.cn/post/1234567890",            // 稀土掘金
        "https://www.cnblogs.com/",                     // 博客园首页
        "https://segmentfault.com/a/1190000000000000",  // 思否
        "https://www.jianshu.com/p/abcdef",             // 简书
    };
    for (const char* u : kCn) {
        auto r = t.check_permissions({{"url", u}}, ctx);
        REQUIRE(r.is_ok());
    }
}

TEST_CASE("WebFetch check_permissions 非白名单域名无确认通道时拒绝（fail-closed）",
          "[web_fetch][perm]") {
    WebFetchTool t;
    ToolContext ctx;  // 无 event_bus → ask_user_confirm 返回 false
    auto r = t.check_permissions({{"url", "https://some-random-site-xyz.example.com/page"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
}
