/**
 * @file test_agent_core.cpp
 * @brief 0.6.x Agent 架构核心测试（#31 目标导向 + #33 类型体系）
 * @details 验证：
 *          - parse_agent_type：agent.active 别名解析（含 skill 过滤兼容）
 *          - parse_goal：agent.goal 目标串解析
 *          - Verdict：check_goal 各类目标验证（FileExists / CustomScript 实证；
 *            TestsPass/BuildClean 因依赖真实构建环境，用可注入命令验证）
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "agent/core/agent_type.h"
#include "agent/core/goal_verdict.h"
#include "agent/core/verdict.h"

namespace fs = std::filesystem;
using namespace agent;

// ============================================================
// parse_agent_type — 类型解析与别名
// ============================================================

TEST_CASE("parse_agent_type: 空串与默认回退 ReAct", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("") == AgentType::ReAct);
    REQUIRE(parse_agent_type("  ") == AgentType::ReAct);
    REQUIRE(parse_agent_type("react") == AgentType::ReAct);
    REQUIRE(parse_agent_type("ReAct") == AgentType::ReAct);
    REQUIRE(parse_agent_type("explore") == AgentType::ReAct);  // #33 角色别名
}

TEST_CASE("parse_agent_type: goal-guarded 别名", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("goal-guarded") == AgentType::GoalGuarded);
    REQUIRE(parse_agent_type("goalguarded") == AgentType::GoalGuarded);
    REQUIRE(parse_agent_type("verify") == AgentType::GoalGuarded);
    REQUIRE(parse_agent_type("  Verify  ") == AgentType::GoalGuarded);  // 大小写+空白
}

TEST_CASE("parse_agent_type: 占位类型路由可识别", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("planner") == AgentType::Planner);
    REQUIRE(parse_agent_type("coordinator") == AgentType::Coordinator);
    REQUIRE(parse_agent_type("researcher") == AgentType::Researcher);
    REQUIRE(parse_agent_type("reviewer") == AgentType::Reviewer);
    REQUIRE(parse_agent_type("batch") == AgentType::Batch);
    REQUIRE(parse_agent_type("watch") == AgentType::Watch);
    // #32：新增 Script 类型
    REQUIRE(parse_agent_type("script") == AgentType::Script);
    REQUIRE(parse_agent_type("Script") == AgentType::Script);
    REQUIRE(parse_agent_type("script-agent") == AgentType::Script);
}

TEST_CASE("parse_agent_type: 未知串 → Unknown（不抛异常）", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("bogus-mode") == AgentType::Unknown);
    REQUIRE(parse_agent_type("666") == AgentType::Unknown);
}

TEST_CASE("to_string / is_implemented 一致", "[agent][agent_type]") {
    REQUIRE(to_string(AgentType::GoalGuarded) == "goal-guarded");
    REQUIRE(is_implemented(AgentType::ReAct));
    REQUIRE(is_implemented(AgentType::GoalGuarded));
    // #32：Batch/Watch/Script 已实现
    REQUIRE(is_implemented(AgentType::Batch));
    REQUIRE(is_implemented(AgentType::Watch));
    REQUIRE(is_implemented(AgentType::Script));
    // #33：五个角色 Agent 已实现
    REQUIRE(is_implemented(AgentType::Coordinator));
    REQUIRE(is_implemented(AgentType::Planner));
    REQUIRE(is_implemented(AgentType::Executor));
    REQUIRE(is_implemented(AgentType::Researcher));
    REQUIRE(is_implemented(AgentType::Reviewer));
    // 后台分发（包装默认底层 Agent）
    REQUIRE(is_implemented(AgentType::Background));
    REQUIRE_FALSE(is_implemented(AgentType::Unknown));
}

TEST_CASE("parse_agent_type: background/bg 别名", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("background") == AgentType::Background);
    REQUIRE(parse_agent_type("bg") == AgentType::Background);
    REQUIRE(parse_agent_type(" Background ") == AgentType::Background);
    REQUIRE(to_string(AgentType::Background) == "background");
}

// ============================================================
// parse_goal — 目标解析
// ============================================================

TEST_CASE("parse_goal: 各类型解析", "[agent][goal][verify]") {
    REQUIRE(parse_goal("").type == AgentGoal::None);
    REQUIRE(parse_goal("  ").type == AgentGoal::None);
    REQUIRE(parse_goal("tests_pass").type == AgentGoal::TestsPass);
    REQUIRE(parse_goal("Tests").type == AgentGoal::TestsPass);
    REQUIRE(parse_goal("build_clean").type == AgentGoal::BuildClean);
    REQUIRE(parse_goal("lint_zero").type == AgentGoal::LintZero);
    REQUIRE(parse_goal("bogus").type == AgentGoal::None);  // 未知回退 None
}

TEST_CASE("parse_goal: file_exists 提取路径", "[agent][goal]") {
    AgentGoal g = parse_goal("file_exists:algo.txt");
    REQUIRE(g.type == AgentGoal::FileExists);
    REQUIRE(g.path == "algo.txt");
}

TEST_CASE("parse_goal: cmd 提取命令", "[agent][goal]") {
    AgentGoal g = parse_goal("cmd:ctest --output-on-failure");
    REQUIRE(g.type == AgentGoal::CustomScript);
    REQUIRE(g.command == "ctest --output-on-failure");
}

// ---- #32 多模式目标语法 ----
TEST_CASE("parse_goal: script 提取命令", "[agent][goal]") {
    AgentGoal g = parse_goal("script:python build.py --all");
    REQUIRE(g.type == AgentGoal::Script);
    REQUIRE(g.command == "python build.py --all");
}

TEST_CASE("parse_goal: batch 解析模板/glob/并发度", "[agent][goal]") {
    AgentGoal g =
        parse_goal("batch:cmd=npm test -- {item}&glob=packages/**/*.test.js&concurrency=4");
    REQUIRE(g.type == AgentGoal::Batch);
    REQUIRE(g.command == "npm test -- {item}");
    REQUIRE(g.glob == "packages/**/*.test.js");
    REQUIRE(g.concurrency == 4);
}

TEST_CASE("parse_goal: batch 缺省字段用默认值", "[agent][goal]") {
    AgentGoal g = parse_goal("batch:cmd=ctest -- {item}");
    REQUIRE(g.type == AgentGoal::Batch);
    REQUIRE(g.command == "ctest -- {item}");
    REQUIRE(g.glob.empty());  // 默认 glob 见 mode_agent_common
    REQUIRE(g.concurrency == 1);
}

TEST_CASE("parse_goal: watch 解析 path/cmd/polls/interval", "[agent][goal]") {
    AgentGoal g = parse_goal("watch:path=src&cmd=cmake --build .&polls=5&interval=200&glob=*.cpp");
    REQUIRE(g.type == AgentGoal::Watch);
    REQUIRE(g.path == "src");
    REQUIRE(g.command == "cmake --build .");
    REQUIRE(g.watch_polls == 5);
    REQUIRE(g.watch_interval_ms == 200);
    REQUIRE(g.glob == "*.cpp");
}

// ============================================================
// Verdict — check_goal 目标验证
// ============================================================

namespace {

inline std::string test_dir() {
    return (fs::temp_directory_path() /
            ("workx_verdict_test_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
        .string();
}

}  // namespace

TEST_CASE("verdict: FileExists 达成/未达成", "[agent][verify]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);
    const std::string target = dir + "/target.txt";
    // 未达成
    AgentGoal missing;
    missing.type = AgentGoal::FileExists;
    missing.path = target;
    Verdict v1 = check_goal(missing, dir);
    REQUIRE(v1.status == GoalStatus::Pending);

    // 达成
    std::ofstream(target) << "x";
    REQUIRE(fs::exists(target));
    Verdict v2 = check_goal(missing, dir);
    REQUIRE(v2.status == GoalStatus::Achieved);

    fs::remove_all(dir);
}

TEST_CASE("verdict: CustomScript 退出码判定", "[agent][verify]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);

    // 成功脚本（退出 0）→ Achieved
    AgentGoal ok_goal;
    ok_goal.type = AgentGoal::CustomScript;
#ifdef _WIN32
    ok_goal.command = "cmd /C exit 0";
#else
    ok_goal.command = "true";
#endif
    Verdict v_ok = check_goal(ok_goal, dir);
    REQUIRE(v_ok.status == GoalStatus::Achieved);

    // 失败脚本（退出 1）→ Pending
    AgentGoal bad_goal;
    bad_goal.type = AgentGoal::CustomScript;
#ifdef _WIN32
    bad_goal.command = "cmd /C exit 1";
#else
    bad_goal.command = "false";
#endif
    Verdict v_bad = check_goal(bad_goal, dir);
    REQUIRE(v_bad.status == GoalStatus::Pending);

    fs::remove_all(dir);
}

TEST_CASE("verdict: None 目标 → Pending", "[agent][verify]") {
    AgentGoal none_goal;  // type = None
    Verdict v = check_goal(none_goal, ".");
    REQUIRE(v.status == GoalStatus::Pending);
}

TEST_CASE("verdict: 缺少验证器/错误配置 → Failed", "[agent][verify]") {
    // file_exists 缺 path → Failed
    AgentGoal no_path;
    no_path.type = AgentGoal::FileExists;
    REQUIRE(check_goal(no_path, ".").status == GoalStatus::Failed);
    // custom_script 缺 command → Failed
    AgentGoal no_cmd;
    no_cmd.type = AgentGoal::CustomScript;
    REQUIRE(check_goal(no_cmd, ".").status == GoalStatus::Failed);
}

// ============================================================
// P1-1 / P2-1 安全加固验证（review R1 注入场景）
// ============================================================

TEST_CASE("P1-1: 白名单外的危险命令被拒绝执行", "[agent][verify][security]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);

    // rm -rf /：不在白名单 → 直接拒绝，不得执行
    AgentGoal rm;
    rm.type = AgentGoal::CustomScript;
    rm.command = "rm -rf /";
    Verdict v_rm = check_goal(rm, dir);
    REQUIRE(v_rm.status == GoalStatus::Failed);
    REQUIRE(v_rm.detail.find("rejected") != std::string::npos);  // 提示被拒绝

    // del / s 等 Windows 危险命令同理
    AgentGoal del;
    del.type = AgentGoal::CustomScript;
    del.command = "del /f /q C:\\";
    Verdict v_del = check_goal(del, dir);
    REQUIRE(v_del.status == GoalStatus::Failed);

    fs::remove_all(dir);
}

TEST_CASE("P1-1: 白名单内命令仍可执行", "[agent][verify][security]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);
#ifdef _WIN32
    const std::string ok_cmd = "cmd /C exit 0";  // 包装 shell 褪去后落点 exit ∈ whitelist
#else
    const std::string ok_cmd = "true";
#endif
    AgentGoal ok;
    ok.type = AgentGoal::CustomScript;
    ok.command = ok_cmd;
    REQUIRE(check_goal(ok, dir).status == GoalStatus::Achieved);
    fs::remove_all(dir);
}

TEST_CASE("P2-1: file_exists 路径逃逸目录被拒绝", "[agent][verify][security]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);
    // 逃逸 cwd 的路径（不管文件是否存在）→ 一律 Failed，防任意文件探测
    AgentGoal escape;
    escape.type = AgentGoal::FileExists;
    escape.path = "../../.ssh/id_rsa";  // 相对逃逸
    REQUIRE(check_goal(escape, dir).status == GoalStatus::Failed);
    // 绝对路径但指向 cwd 外
    AgentGoal abs_escape;
    abs_escape.type = AgentGoal::FileExists;
    abs_escape.path = "/etc/passwd";
    REQUIRE(check_goal(abs_escape, dir).status == GoalStatus::Failed);
    fs::remove_all(dir);
}

TEST_CASE("P2-3: file_exists 路径保留原始大小写", "[agent][goal][verify]") {
    // 前缀大小写不敏感，但路径值大小写原样保留（P2-3）
    AgentGoal g_mixed = parse_goal("FILE_EXISTS:Algo.Txt");
    REQUIRE(g_mixed.type == AgentGoal::FileExists);
    REQUIRE(g_mixed.path == "Algo.Txt");
    AgentGoal g_cmd = parse_goal("CMD:ctest --output-on-failure");
    REQUIRE(g_cmd.type == AgentGoal::CustomScript);
    REQUIRE(g_cmd.command == "ctest --output-on-failure");
}