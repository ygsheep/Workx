/**
 * @file test_sandbox_visibility.cpp
 * @brief 沙箱状态可见化单元测试（#84 方案 A）
 * @details 覆盖：
 *          - 三态判定（classify_sandbox）：active / degraded / disabled
 *          - 进度文案：active 保持既有格式，degraded / disabled 明确标注"未经隔离"
 *          - 审计详情：platform / requested_level / actual_backend / reason
 *          - 去重：同一实例内降级与关闭各只留痕一条，进度仍逐次上报
 *          - active 不写审计
 *          - BashTool 端到端：执行命令时真的会上报沙箱状态
 *
 *          审计断言读真实的 audit.jsonl（临时目录），不做替身 ——
 *          否则失去"降级确实被记录在案"这一最关键的证据。
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/audit/audit_logger.h"
#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/ShellTool/sandbox_visibility.h"
#include "agent/tool/context.h"
#include "core/config/config_manager.h"
#include "core/process/sandbox/sandbox_adapter.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::tool::shell_common;
using process::sandbox::WrappedCommand;

namespace {

WrappedCommand active_cmd() {
    WrappedCommand w;
    w.cmd = "bwrap";
    w.was_wrapped = true;
    w.degraded = false;
    w.backend_name = "bubblewrap";
    return w;
}

WrappedCommand degraded_cmd() {
    WrappedCommand w;
    w.cmd = "cmd.exe";
    w.was_wrapped = false;
    w.degraded = true;
    w.backend_name = "none";
    return w;
}

/// 宽松配置下的直通结果（backend_name 为空）
WrappedCommand passthrough_cmd() {
    WrappedCommand w;
    w.cmd = "cmd.exe";
    w.was_wrapped = false;
    w.degraded = false;
    return w;
}

/// @brief 临时审计文件 + 进度收集 fixture
class VisibilityFixture {
   public:
    std::filesystem::path temp_dir;
    std::filesystem::path audit_file;
    std::vector<std::string> progress;

    VisibilityFixture() {
        temp_dir = std::filesystem::temp_directory_path() /
                   ("workx_sandbox_vis_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(temp_dir);
        audit_file = temp_dir / "audit.jsonl";
        audit::AuditLogger::instance().init(audit_file.string());
    }

    ~VisibilityFixture() {
        audit::AuditLogger::instance().set_enabled(false);
        std::error_code ec;
        std::filesystem::remove_all(temp_dir, ec);
    }

    /// @brief 填充最小可用 ToolContext（含进度回调）
    void fill_ctx(ToolContext& ctx) {
        ctx.cwd = std::filesystem::current_path().string();
        ctx.session_id = "issue84-test";
        ctx.config_manager_ptr = &ConfigManager::instance();
        ctx.progress_callback = [this](const std::string& text) { progress.push_back(text); };
    }

    /// @brief 读取审计文件里指定 event_type 的记录
    std::vector<nlohmann::json> events_of(std::string_view type) const {
        std::vector<nlohmann::json> out;
        std::ifstream ifs(audit_file);
        std::string line;
        while (std::getline(ifs, line)) {
            if (line.empty()) continue;
            auto j = nlohmann::json::parse(line);
            if (j.value("event_type", std::string{}) == type) out.push_back(std::move(j));
        }
        return out;
    }
};

}  // namespace

// ============================================================
// 纯函数：状态判定
// ============================================================

TEST_CASE("沙箱三态判定：active / degraded / disabled", "[sandbox_visibility][issue84]") {
    REQUIRE(classify_sandbox(active_cmd(), false) == SandboxState::Active);
    REQUIRE(classify_sandbox(degraded_cmd(), false) == SandboxState::Degraded);
    REQUIRE(classify_sandbox(passthrough_cmd(), true) == SandboxState::Disabled);
    // 未包装、未降级、也非显式关闭：按"无隔离"如实上报，宁可误报不可漏报
    REQUIRE(classify_sandbox(passthrough_cmd(), false) == SandboxState::Degraded);
}

// ============================================================
// 纯函数：进度文案
// ============================================================

TEST_CASE("进度文案保留 active 原格式并显式标注未隔离", "[sandbox_visibility][issue84]") {
    REQUIRE(sandbox_progress_message(SandboxState::Active, active_cmd()) ==
            "Sandbox: active (backend: bubblewrap)");

    const auto degraded = sandbox_progress_message(SandboxState::Degraded, degraded_cmd());
    REQUIRE(degraded.rfind("Sandbox: degraded (backend: none)", 0) == 0);
    REQUIRE(degraded.find("WITHOUT OS-level isolation") != std::string::npos);

    const auto disabled = sandbox_progress_message(SandboxState::Disabled, passthrough_cmd());
    REQUIRE(disabled.rfind("Sandbox: disabled", 0) == 0);
    REQUIRE(disabled.find("WITHOUT OS-level isolation") != std::string::npos);
}

// ============================================================
// 纯函数：审计详情
// ============================================================

TEST_CASE("审计详情带平台、请求档位与实际后端", "[sandbox_visibility][issue84]") {
    const auto degraded = sandbox_audit_detail(SandboxState::Degraded, degraded_cmd());
    REQUIRE(degraded.rfind("platform=", 0) == 0);
    REQUIRE(degraded.find("requested_level=restrictive") != std::string::npos);
    REQUIRE(degraded.find("actual_backend=none") != std::string::npos);

    const auto disabled = sandbox_audit_detail(SandboxState::Disabled, passthrough_cmd());
    REQUIRE(disabled.find("requested_level=permissive") != std::string::npos);
    REQUIRE(disabled.find("reason=dangerously_disable_sandbox") != std::string::npos);
}

// ============================================================
// 上报器：审计去重
// ============================================================

TEST_CASE("降级审计只留痕一次而进度逐次上报", "[sandbox_visibility][issue84]") {
    VisibilityFixture fix;
    ToolContext ctx;
    fix.fill_ctx(ctx);
    SandboxVisibility vis;

    for (int i = 0; i < 3; ++i) {
        vis.report(degraded_cmd(), false, "Bash", ctx);
    }

    REQUIRE(fix.progress.size() == 3);
    REQUIRE(fix.events_of("security.sandbox_degraded").size() == 1);
}

TEST_CASE("降级与显式关闭各留一条互不吞并", "[sandbox_visibility][issue84]") {
    VisibilityFixture fix;
    ToolContext ctx;
    fix.fill_ctx(ctx);
    SandboxVisibility vis;

    vis.report(degraded_cmd(), false, "Bash", ctx);
    vis.report(degraded_cmd(), false, "Bash", ctx);
    vis.report(passthrough_cmd(), true, "Bash", ctx);
    vis.report(passthrough_cmd(), true, "Bash", ctx);

    const auto degraded = fix.events_of("security.sandbox_degraded");
    const auto disabled = fix.events_of("security.sandbox_disabled");
    REQUIRE(degraded.size() == 1);
    REQUIRE(disabled.size() == 1);

    // 事件里带上工具名与可归因的详情
    REQUIRE(disabled.front()["tool_name"] == "Bash");
    REQUIRE(disabled.front()["session_id"] == "issue84-test");
    const std::string detail = disabled.front()["input"]["detail"];
    REQUIRE(detail.find("requested_level=permissive") != std::string::npos);
}

TEST_CASE("沙箱真正生效时不写任何降级审计", "[sandbox_visibility][issue84]") {
    VisibilityFixture fix;
    ToolContext ctx;
    fix.fill_ctx(ctx);
    SandboxVisibility vis;

    vis.report(active_cmd(), false, "Bash", ctx);
    vis.report(active_cmd(), false, "Bash", ctx);

    REQUIRE(fix.progress.size() == 2);
    REQUIRE(fix.events_of("security.sandbox_degraded").empty());
    REQUIRE(fix.events_of("security.sandbox_disabled").empty());
}

// ============================================================
// 端到端：BashTool 真的会上报
// ============================================================

TEST_CASE("BashTool 执行命令时上报真实沙箱状态", "[sandbox_visibility][issue84]") {
    VisibilityFixture fix;
    ToolContext ctx;
    fix.fill_ctx(ctx);

    BashTool tool;
    auto r = tool.call(nlohmann::json{{"command", "echo issue84"}}, ctx);
    REQUIRE(r.is_ok());

    bool reported = false;
    for (const auto& text : fix.progress) {
        if (text.rfind("Sandbox: ", 0) == 0) {
            reported = true;
            break;
        }
    }
    REQUIRE(reported);
}
