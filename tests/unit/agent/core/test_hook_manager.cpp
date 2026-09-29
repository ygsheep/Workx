/**
 * @file test_hook_manager.cpp
 * @brief Issue #50 通用 Hook 事件系统 — HookEvent / HookMatcher / HookManager 单元测试
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/hook/hook_event.h"
#include "agent/hook/hook_match.h"
#include "agent/hook/hook_manager.h"
#include "agent/skill/inclaude/frontmatter.h"  // frontmatter 对象式 hooks 解析
#include "core/events/agent_events.h"          // HookProgressEvent（M-2）
#include "helpers/mock_config_manager.h"       // MockConfigManager（agent 类型子循环装配）
#include "helpers/mock_event_bus.h"            // MockEventBus（M-2 订阅断言）
#include "helpers/mock_provider.h"

using namespace agent::hook;

// ============================================================
// 事件枚举与字符串
// ============================================================

TEST_CASE("hook event name round-trip", "[hook][event]") {
    REQUIRE(std::string(to_string(HookEvent::PreToolUse)) == "PreToolUse");
    REQUIRE(std::string(to_string(HookEvent::PostToolUse)) == "PostToolUse");
    REQUIRE(std::string(to_string(HookEvent::Stop)) == "Stop");
    REQUIRE(std::string(to_string(HookEvent::SessionStart)) == "SessionStart");

    REQUIRE(parse_event("PreToolUse") == HookEvent::PreToolUse);
    REQUIRE(parse_event("pretooluse") == HookEvent::PreToolUse);  // 不敏感
    REQUIRE(parse_event("stop") == HookEvent::Stop);
    REQUIRE(parse_event("Nope") == std::nullopt);
}

TEST_CASE("hook type string", "[hook][event]") {
    REQUIRE(std::string(type_to_string(HookType::Command)) == "command");
    REQUIRE(std::string(type_to_string(HookType::Http)) == "http");
    REQUIRE(std::string(type_to_string(HookType::Prompt)) == "prompt");
    REQUIRE(std::string(type_to_string(HookType::Agent)) == "agent");
}

// ============================================================
// HookMatcher（if 条件匹配）
// ============================================================

TEST_CASE("hook matcher default matches all", "[hook][match]") {
    HookMatcher m("");
    REQUIRE(m.matches_all());
    REQUIRE(m.matches("PreToolUse", "Bash", nlohmann::json::object()));
}

TEST_CASE("hook matcher tool-name only", "[hook][match]") {
    HookMatcher m("Bash");
    REQUIRE(m.matches("PreToolUse", "Bash", nlohmann::json::object()));
    REQUIRE_FALSE(m.matches("PreToolUse", "Read", nlohmann::json::object()));
}

TEST_CASE("hook matcher tool with arg glob", "[hook][match]") {
    HookMatcher m("Bash(rm -rf *)");
    nlohmann::json input = {{"command", "rm -rf /tmp/x"}};
    REQUIRE(m.matches("PreToolUse", "Bash", input));

    nlohmann::json safe = {{"command", "ls -la"}};
    REQUIRE_FALSE(m.matches("PreToolUse", "Bash", safe));
}

TEST_CASE("hook matcher wildcard prefix", "[hook][match]") {
    HookMatcher m("Write(**)");
    nlohmann::json input = {{"path", "a/b/c.txt"}};
    REQUIRE(m.matches("PreToolUse", "Write", input));
}

TEST_CASE("hook matcher OR clauses", "[hook][match]") {
    HookMatcher m("Bash || Read");
    REQUIRE(m.matches("PreToolUse", "Read", nlohmann::json::object()));
    REQUIRE(m.matches("PreToolUse", "Bash", nlohmann::json::object()));
    REQUIRE_FALSE(m.matches("PreToolUse", "Write", nlohmann::json::object()));
}

// ============================================================
// command 类型 hook 执行（跨平台 shell）
// ============================================================

TEST_CASE("hook manager dispatch command succeeds on cross-platform echo", "[hook][command]") {
    HookManager mgr;
    HookDefinition def;
    def.event = HookEvent::Stop;
    def.type = HookType::Command;
#if defined(_WIN32)
    def.command = "echo hook-ok";
#else
    def.command = "echo hook-ok";
#endif
    mgr.register_hook(def);
    REQUIRE(mgr.size() == 1);

    HookContext ctx;
    ctx.session_id = "s1";
    ctx.cwd = ".";
    auto r = mgr.dispatch(HookEvent::Stop, ctx);
    // 成功执行：message 以 [hook:command] ok 开头
    REQUIRE(r.message.find("[hook:command] ok") == 0);
}

TEST_CASE("hook manager empty dispatch returns default", "[hook][dispatch]") {
    HookManager mgr;
    REQUIRE(mgr.empty());
    HookContext ctx;
    HookResult r = mgr.dispatch(HookEvent::Stop, ctx);
    REQUIRE(r.message.empty());
    REQUIRE_FALSE(r.blockingError.has_value());
}

// ============================================================
// 阻止语义（blockingError）
// ============================================================

TEST_CASE("hook manager respects once flag", "[hook][once]") {
    HookManager mgr;
    HookDefinition def;
    def.event = HookEvent::Stop;
    def.type = HookType::Command;
    def.once = true;
#if defined(_WIN32)
    def.command = "cd .";  // 成功退出码
#else
    def.command = ":";  // 成功，无副作用
#endif
    mgr.register_hook(def);

    HookContext ctx;
    ctx.cwd = ".";
    auto r1 = mgr.dispatch(HookEvent::Stop, ctx);
    REQUIRE(r1.message.find("[hook:command] ok") == 0);
    // once：第二次不再执行 → message 为空
    auto r2 = mgr.dispatch(HookEvent::Stop, ctx);
    REQUIRE(r2.message.empty());
}

// ============================================================
// HookDefinition::from_json
// ============================================================

TEST_CASE("hook definition from json", "[hook][json]") {
    nlohmann::json obj = {
        {"event", "PreToolUse"},  {"type", "http"},
        {"match", "Bash(git *)"}, {"url", "http://localhost:9999/hook"},
        {"timeout_ms", 5000},     {"once", true},
    };
    auto def = HookDefinition::from_json(obj);
    REQUIRE(def.event == HookEvent::PreToolUse);
    REQUIRE(def.type == HookType::Http);
    REQUIRE(def.match == "Bash(git *)");
    REQUIRE(def.url == "http://localhost:9999/hook");
    REQUIRE(def.timeout_ms == 5000);
    REQUIRE(def.once);
    REQUIRE_FALSE(def.async);
}

TEST_CASE("hook definition from json defaults", "[hook][json]") {
    nlohmann::json obj = {{"event", "Stop"}, {"type", "command"}, {"command", "echo hi"}};
    auto def = HookDefinition::from_json(obj);
    REQUIRE(def.event == HookEvent::Stop);
    REQUIRE(def.type == HookType::Command);
    REQUIRE(def.timeout_ms == 30000);  // 默认
    REQUIRE_FALSE(def.once);
    REQUIRE(def.match.empty());
}

// ============================================================
// prompt 类型 hook（依赖注入的 LLM provider）
// ============================================================

TEST_CASE("hook prompt without provider degrades gracefully", "[hook][prompt]") {
    HookManager mgr;
    HookDefinition def;
    def.event = HookEvent::PreToolUse;
    def.type = HookType::Prompt;
    def.prompt = "is this allowed?";
    mgr.register_hook(def);

    HookContext ctx;
    ctx.cwd = ".";
    auto r = mgr.dispatch(HookEvent::PreToolUse, ctx);
    REQUIRE(r.message.find("[hook:prompt] not ready") == 0);
    REQUIRE_FALSE(r.blockingError.has_value());
}

TEST_CASE("hook prompt JSON verdict blocks tool", "[hook][prompt]") {
    using agent::test::MockCompletionProvider;
    using agent::test::MockStreamReader;

    MockCompletionProvider provider;
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("{\"blockingError\":\"危险操作\"}");
    provider.set_next_reader(reader);

    HookManager mgr;
    mgr.set_provider(&provider);
    HookDefinition def;
    def.event = HookEvent::PreToolUse;
    def.type = HookType::Prompt;
    def.prompt = "gate dangerous bash";
    mgr.register_hook(def);

    HookContext ctx;
    ctx.cwd = ".";
    ctx.tool_name = "Bash";
    ctx.tool_input = {{"command", "rm -rf /"}};
    auto r = mgr.dispatch(HookEvent::PreToolUse, ctx);
    REQUIRE(r.blockingError.has_value());
    REQUIRE(r.blockingError->find("危险操作") != std::string::npos);
    REQUIRE(provider.submit_count == 1);
}

TEST_CASE("hook prompt allow verdict not blocking", "[hook][prompt]") {
    using agent::test::MockCompletionProvider;
    using agent::test::MockStreamReader;

    MockCompletionProvider provider;
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("{}");
    provider.set_next_reader(reader);

    HookManager mgr;
    mgr.set_provider(&provider);
    HookDefinition def;
    def.event = HookEvent::PreToolUse;
    def.type = HookType::Prompt;
    mgr.register_hook(def);

    HookContext ctx;
    ctx.cwd = ".";
    auto r = mgr.dispatch(HookEvent::PreToolUse, ctx);
    REQUIRE_FALSE(r.blockingError.has_value());
    REQUIRE_FALSE(r.preventContinuation);
}

// ============================================================
// agent 类型：受限子 ReActLoop 做 agentic verifier
// ============================================================

TEST_CASE("hook agent without deps degrades gracefully", "[hook][agent]") {
    HookManager mgr;  // 未注入 provider / config_manager
    HookDefinition def;
    def.event = HookEvent::PreToolUse;
    def.type = HookType::Agent;
    def.prompt = "verify";
    mgr.register_hook(def);

    HookContext ctx;
    ctx.cwd = ".";
    auto r = mgr.dispatch(HookEvent::PreToolUse, ctx);
    REQUIRE(r.message.find("[hook:agent] not ready") == 0);
    REQUIRE_FALSE(r.blockingError.has_value());
}

TEST_CASE("hook agent empty prompt skipped", "[hook][agent]") {
    using agent::test::MockCompletionProvider;
    using agent::test::MockConfigManager;

    MockCompletionProvider provider;
    MockConfigManager cfg;
    HookManager mgr;
    mgr.set_provider(&provider);
    mgr.set_config_manager(&cfg);

    HookDefinition def;
    def.event = HookEvent::PreToolUse;
    def.type = HookType::Agent;
    mgr.register_hook(def);  // 空 prompt

    HookContext ctx;
    ctx.cwd = ".";
    auto r = mgr.dispatch(HookEvent::PreToolUse, ctx);
    REQUIRE(r.message.find("[hook:agent] empty prompt") == 0);
    REQUIRE(provider.submit_count == 0);  // 未触发任何子循环
}

TEST_CASE("hook agent JSON verdict blocks tool", "[hook][agent]") {
    using agent::test::MockCompletionProvider;
    using agent::test::MockConfigManager;
    using agent::test::MockStreamReader;

    MockCompletionProvider provider;
    // 无白名单 registry 注入 → 子循环空工具集，verifier 直接给出最终 JSON 判定
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("{\"blockingError\":\"agent 判定危险\"}");
    provider.set_next_reader(reader);

    MockConfigManager cfg;
    HookManager mgr;
    mgr.set_provider(&provider);
    mgr.set_config_manager(&cfg);

    HookDefinition def;
    def.event = HookEvent::PreToolUse;
    def.type = HookType::Agent;
    def.prompt = "verify bash command";
    mgr.register_hook(def);

    HookContext ctx;
    ctx.cwd = ".";
    ctx.tool_name = "Bash";
    ctx.tool_input = {{"command", "rm -rf /"}};
    auto r = mgr.dispatch(HookEvent::PreToolUse, ctx);
    REQUIRE(r.blockingError.has_value());
    REQUIRE(r.blockingError->find("agent 判定危险") != std::string::npos);
    REQUIRE(provider.submit_count == 1);  // 仅一次子循环 LLM 调用
}

// ============================================================
// M-2：hook 进度事件（经 IEventBus 发布）
// ============================================================

TEST_CASE("hook dispatch publishes HookProgressEvent (M-2)", "[hook][progress]") {
    using agent::test::MockEventBus;
    MockEventBus bus;
    bus.set_dispatch_enabled(true);  // 让订阅回调在 publish_raw 时被同步调用

    HookManager mgr;
    mgr.set_event_bus(&bus);
    HookDefinition def;
    def.event = HookEvent::Stop;
    def.type = HookType::Command;
#if defined(_WIN32)
    def.command = "cd .";  // 成功退出码
#else
    def.command = ":";  // 成功，无副作用
#endif
    mgr.register_hook(def);

    int start_count = 0;
    int done_count = 0;
    std::string last_hook_type;
    std::string last_hook_label;
    uint64_t start_hook_id = 0;
    uint64_t done_hook_id = 0;
    bus.subscribe<agent::HookProgressEvent>([&](const agent::HookProgressEvent& ev) {
        last_hook_type = ev.hook_type;
        last_hook_label = ev.hook_label;
        if (ev.phase == "start") {
            ++start_count;
            start_hook_id = ev.hook_id;
        } else if (ev.phase == "done") {
            ++done_count;
            done_hook_id = ev.hook_id;
        }
    });

    HookContext ctx;
    ctx.session_id = "s1";
    ctx.cwd = ".";
    ctx.tool_name = "SomeTool";
    auto r = mgr.dispatch(HookEvent::Stop, ctx);
    REQUIRE(r.message.find("[hook:command] ok") == 0);

    bus.drain_async_events(8);
    REQUIRE(start_count == 1);  // 每次 hook 执行发布 1 次 start
    REQUIRE(done_count == 1);   // ……并发布 1 次 done
    REQUIRE(last_hook_type == "command");
    REQUIRE(last_hook_label.find("[c] ") == 0);  // command 标签以 [c] 前缀
    REQUIRE(start_hook_id != 0);                 // bus 注入了 hook_id
    REQUIRE(done_hook_id == start_hook_id);      // start/done 关联同一条执行
}

// ============================================================
// frontmatter 对象式 hooks
// ============================================================

TEST_CASE("frontmatter object hooks parsed to JSON", "[skill][frontmatter][hook]") {
    const std::string content = R"fm(---
hooks:
  - event: PreToolUse
    type: command
    match: "Bash(rm *)"
    command: "echo not allowed"
    blockingError: "禁止执行 rm"
    timeout: 5000
  - event: PostToolUse
    type: prompt
    prompt: "check result"
---
Skill body
)fm";

    const auto parsed = agent::skill::parse_skill_content(content, "test-skill");
    REQUIRE(parsed.frontmatter.hooks.empty());  // 传统 command hooks 为空
    REQUIRE(parsed.frontmatter.hooks_json.size() == 1);

    auto json = nlohmann::json::parse(parsed.frontmatter.hooks_json[0]);
    REQUIRE(json.is_array());
    REQUIRE(json.size() == 2);

    REQUIRE(json[0]["event"] == "PreToolUse");
    REQUIRE(json[0]["type"] == "command");
    REQUIRE(json[0]["match"] == "Bash(rm *)");  // 括号保留
    REQUIRE(json[0]["command"] == "echo not allowed");
    REQUIRE(json[0]["blockingError"] == "禁止执行 rm");
    REQUIRE(json[0]["timeout"] == 5000);  // 整数推断

    REQUIRE(json[1]["event"] == "PostToolUse");
    REQUIRE(json[1]["type"] == "prompt");
    REQUIRE(json[1]["prompt"] == "check result");
}

TEST_CASE("frontmatter mix object and command hooks", "[skill][frontmatter][hook]") {
    const std::string content = R"fm(---
hooks:
  - echo preactivate
  - event: PreToolUse
    type: command
    match: "Bash(rm *)"
    command: "echo blocked"
  - event: PostToolUse
    type: agent
    prompt: "verify"
---
Skill body
)fm";

    const auto parsed = agent::skill::parse_skill_content(content, "mix-skill");
    // 传统 command hook 保留，对象式 hook 归入 hooks_json
    REQUIRE(parsed.frontmatter.hooks.size() == 1);
    REQUIRE(parsed.frontmatter.hooks[0] == "echo preactivate");
    REQUIRE(parsed.frontmatter.hooks_json.size() == 1);

    auto json = nlohmann::json::parse(parsed.frontmatter.hooks_json[0]);
    REQUIRE(json.size() == 2);
    REQUIRE(json[0]["event"] == "PreToolUse");
    REQUIRE(json[1]["event"] == "PostToolUse");
    REQUIRE(json[1]["type"] == "agent");
}