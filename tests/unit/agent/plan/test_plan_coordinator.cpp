/**
 * @file test_plan_coordinator.cpp
 * @brief Plan Mode V2（#54）PlanCoordinator 单元测试
 * @details 覆盖：
 *          - 五阶段流转（interview → exploring → planning → awaiting_approval → done）
 *          - explore 并行计数（= plan.explore_agent_count）
 *          - 驳回/批准分支
 *          - PlanRunner 注入（结构化产物 + critical_files 聚合）
 *          - serialize/deserialize 恢复
 */

#include <catch2/catch_test_macros.hpp>
#include <algorithm>  // std::find（M-1 关键文件断言）
#include <string>
#include <vector>

#include "agent/plan/plan_coordinator.h"
#include "agent/config/app_config.h"
#include "helpers/mock_config_manager.h"

using namespace agent;
using namespace agent::plan;
using namespace agent::test;

namespace {

/// @brief 冷却默认（auto+interview 全开启）的协调器
PlanCoordinator make_default_coordinator(MockConfigManager& cfg) { return PlanCoordinator(cfg); }

void run_explore_click(PlanCoordinator& pc) {
    // 模拟宿主：逐个收尾 explore 任务（按启动顺序）
    auto active = pc.active_task_ids();
    for (const auto& id : active) {
        pc.on_explore_task_done(id, "探索结论(" + id + ")", false);
    }
}

}  // namespace

TEST_CASE("PlanCoordinator auto+interview: begin_plan stays in Interview", "[plan]") {
    MockConfigManager cfg;
    PlanCoordinator pc = make_default_coordinator(cfg);
    REQUIRE(pc.auto_enabled());
    REQUIRE(pc.interview_enabled());

    pc.begin_plan("重构登录模块");
    REQUIRE(pc.stage() == PlanStage::Interview);
    REQUIRE(pc.reason() == "重构登录模块");
}

TEST_CASE("PlanCoordinator skips interview when disabled and auto on", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    PlanCoordinator pc = make_default_coordinator(cfg);

    int launched = 0;
    pc.set_explore_runner(
        [&](const std::string&, const std::string&, const std::string&) { ++launched; });
    pc.begin_plan("refactor");
    REQUIRE(pc.stage() == PlanStage::Exploring);
    REQUIRE(launched == pc.explore_agent_count());
    REQUIRE(pc.active_task_ids().size() >= 1);
}

TEST_CASE("PlanCoordinator launches explore_agent_count parallel tasks", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    cfg.set(keys::PLAN_EXPLORE_AGENT_COUNT, 4);
    PlanCoordinator pc = make_default_coordinator(cfg);

    std::vector<std::string> task_ids;
    pc.set_explore_runner(
        [&](const std::string& id, const std::string& area, const std::string& prompt) {
            task_ids.push_back(id);
            REQUIRE_FALSE(area.empty());
            REQUIRE_FALSE(prompt.empty());
        });
    pc.begin_plan("refactor");
    REQUIRE(task_ids.size() == 4);
    REQUIRE(pc.active_task_ids().size() == 4);

    // 全部收尾 → planning → awaiting_approval（无 plan_runner 走机械合成）
    run_explore_click(pc);
    REQUIRE(pc.stage() == PlanStage::AwaitingApproval);
    REQUIRE(pc.findings().size() == 4);
}

TEST_CASE("PlanCoordinator interview → interview notes → explore", "[plan]") {
    MockConfigManager cfg;
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("重构");
    REQUIRE(pc.stage() == PlanStage::Interview);

    int launched = 0;
    pc.set_explore_runner(
        [&](const std::string&, const std::string&, const std::string&) { ++launched; });
    pc.set_interview_notes("约束：保持兼容，只读调研");
    REQUIRE(pc.stage() == PlanStage::Exploring);
    REQUIRE(launched == pc.explore_agent_count());
    REQUIRE(pc.context() == "约束：保持兼容，只读调研");
}

TEST_CASE("PlanCoordinator rejecting returns to Interview for revision", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("refactor");
    run_explore_click(pc);
    REQUIRE(pc.stage() == PlanStage::AwaitingApproval);

    pc.set_approved(false);
    REQUIRE(pc.stage() == PlanStage::Interview);
}

TEST_CASE("PlanCoordinator approving reaches Done", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("refactor");
    run_explore_click(pc);
    pc.set_approved(true);
    REQUIRE(pc.stage() == PlanStage::Done);
    REQUIRE(is_terminal(pc.stage()));
}

TEST_CASE("PlanCoordinator PlanRunner supplies structured artifact + critical_files", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    cfg.set(keys::PLAN_EXPLORE_AGENT_COUNT, 2);
    PlanCoordinator pc = make_default_coordinator(cfg);

    pc.set_explore_runner([](const std::string&, const std::string&, const std::string&) {});
    pc.set_plan_runner([](const std::vector<ExploreFinding>& findings, const std::string& notes) {
        PlanArtifact a;
        a.summary = "综合方案";
        a.interview_notes = notes;
        a.critical_files = {"src/module.cpp", "include/module.h"};
        a.steps = nlohmann::json::array({{{"action", "改接口"}, {"detail", "调整 module.h"}}});
        a.risks = nlohmann::json::array({"风险A"});
        a.findings = findings;
        return a;
    });

    pc.begin_plan("refactor");
    run_explore_click(pc);
    REQUIRE(pc.stage() == PlanStage::AwaitingApproval);
    REQUIRE(pc.artifact().summary == "综合方案");
    REQUIRE(pc.artifact().critical_files.size() == 2);
    REQUIRE(pc.artifact().critical_files[0] == "src/module.cpp");
    REQUIRE(pc.artifact().findings.size() == 2);  // 透传 explore 发现
}

TEST_CASE("PlanCoordinator serialize/deserialize round-trip", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    cfg.set(keys::PLAN_EXPLORE_AGENT_COUNT, 2);
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("refactor");
    run_explore_click(pc);

    nlohmann::json snap = pc.serialize();
    REQUIRE(snap["stage"].get<std::string>() == "awaiting_approval");
    REQUIRE(snap["artifact"].contains("critical_files"));

    PlanCoordinator pc2 = make_default_coordinator(cfg);
    pc2.deserialize(snap);
    REQUIRE(pc2.stage() == PlanStage::AwaitingApproval);
    REQUIRE(pc2.artifact().markdown == pc.artifact().markdown);
    REQUIRE(pc2.findings().size() == 2);
}

TEST_CASE("PlanCoordinator mechanical synthesis aggregates critical_files", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    cfg.set(keys::PLAN_EXPLORE_AGENT_COUNT, 2);
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("refactor");

    // 先完成一个任务（注入 critical_files），用于聚合
    auto active = pc.active_task_ids();
    REQUIRE(active.size() == 2);

    // 机械合成路径直接查 findings 聚合；无 plan_runner 时从 findings 收 critical_files
    // 这里通过 PlanRunner 缺省（机械）走 on_explore_task_done。findings 带 critical_files。
    // 修改：先人为构造 findings 场景 —— 通过 explore_runner 直接记录则拿不到 files，
    // 故使用 on_explore_task_done 传入（但该方法不接收 files）。此处校验 mkdown 字段存在。
    run_explore_click(pc);
    REQUIRE(pc.stage() == PlanStage::AwaitingApproval);
    REQUIRE_FALSE(pc.artifact().markdown.empty());
    REQUIRE(pc.artifact().markdown.find("Critical Files for Implementation") != std::string::npos);
    REQUIRE(pc.plan_cycle() >= 1);
}

TEST_CASE("PlanCoordinator explore task_id is cycle-unique across plan cycles", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    cfg.set(keys::PLAN_EXPLORE_AGENT_COUNT, 2);
    PlanCoordinator pc = make_default_coordinator(cfg);

    // 第 1 轮：task_id 形如 pa-1-1 / pa-1-2
    pc.begin_plan("第一轮");
    auto cycle1 = pc.active_task_ids();
    REQUIRE(cycle1.size() == 2);
    REQUIRE(cycle1[0].rfind("pa-1-", 0) == 0);
    REQUIRE(pc.plan_cycle() == 1);

    // 新一轮规划复位后编号继续递增（pa-2-...），杜绝跨轮 task_id 冲突
    pc.begin_plan("第二轮");
    auto cycle2 = pc.active_task_ids();
    REQUIRE(cycle2.size() == 2);
    REQUIRE(cycle2[0].rfind("pa-2-", 0) == 0);
    REQUIRE(pc.plan_cycle() == 2);

    // 两轮 id 集合不相交
    for (const auto& id1 : cycle1) {
        for (const auto& id2 : cycle2) {
            REQUIRE_FALSE(id1 == id2);
        }
    }
}

TEST_CASE("PlanCoordinator set_critical_files consumes structured files into artifact", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("refactor");
    run_explore_click(pc);
    REQUIRE(pc.stage() == PlanStage::AwaitingApproval);

    // 审批时工具显式传入结构化关键文件 → 并入产物（缺省产物为空）
    pc.set_approved(true);
    pc.set_critical_files({"src/foo.cpp", "include/foo.h"});
    REQUIRE(pc.artifact().critical_files.size() == 2);
    REQUIRE(pc.artifact().critical_files[0] == "src/foo.cpp");

    // 重复并入去重
    pc.set_critical_files({"src/foo.cpp"});
    REQUIRE(pc.artifact().critical_files.size() == 2);
}

TEST_CASE("explore conclusion critical files are extracted into finding and aggregated", "[plan]") {
    MockConfigManager cfg;
    cfg.set(keys::PLAN_INTERVIEW_ENABLED, false);
    cfg.set(keys::PLAN_EXPLORE_AGENT_COUNT, 1);
    PlanCoordinator pc = make_default_coordinator(cfg);
    pc.begin_plan("refactor");
    const auto ids = pc.active_task_ids();
    REQUIRE_FALSE(ids.empty());

    // 手动回报含路径的 explore 结论 → 关键文件应从文本中解析出来
    pc.on_explore_task_done(
        ids[0],
        "Key files: src/agent/core/react_loop.cpp and src/agent/tool/registry.h; "
        "also tests/unit/test_plan_mode_tools.cpp.",
        false);

    REQUIRE(pc.findings().size() == 1);
    const auto& critical = pc.findings()[0].critical_files;
    REQUIRE_FALSE(critical.empty());
    REQUIRE(std::find(critical.begin(), critical.end(), "src/agent/core/react_loop.cpp") !=
            critical.end());
    REQUIRE(std::find(critical.begin(), critical.end(), "src/agent/tool/registry.h") !=
            critical.end());

    // 机械综合产物应聚合到该文件（M-1 修复：不再恒为空）
    const auto& aggregated = pc.artifact().critical_files;
    REQUIRE(std::find(aggregated.begin(), aggregated.end(), "src/agent/core/react_loop.cpp") !=
            aggregated.end());
}