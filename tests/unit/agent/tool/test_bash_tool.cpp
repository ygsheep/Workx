/**
 * @file test_bash_tool.cpp
 * @brief BashTool 单元测试
 * @details 覆盖：
 *          - 元信息（name/description/prompt/schema）
 *          - 参数校验（缺 command / 空 command）
 *          - 同步执行（echo 命令）
 *          - 超时/取消路径
 *          - 后台任务（TaskManager nullptr 检查）
 *          - 进度回调（report_progress 触发）
 *
 *          跨平台命令选择：
 *          - Windows: cmd.exe /c echo hello
 *          - POSIX:   /bin/sh -c "echo hello"
 *
 *          所有测试都用 PATH 中必定存在的命令，避免依赖项目布局。
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/ShellTool/shell_detector.h"  // detect()：按实际 shell 选命令
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/task/task_manager.h"
#include "core/events/event_bus.h"
#include "core/config/config_manager.h"
#include "core/utils/error.h"

using namespace agent;
using namespace agent::tool;
using namespace std::chrono_literals;

namespace {

/// 构建最小可用 ToolContext（注入 ConfigManager 单例，无 TaskManager）
/// 注意：ToolContext 含 std::atomic 不可拷贝，需通过传入引用方式构造
void fill_ctx(ToolContext& ctx) {
    ctx.cwd = std::filesystem::current_path().string();
    ctx.session_id = "test";
    ctx.config_manager_ptr = &ConfigManager::instance();
    // task_manager_ptr 故意留 nullptr（后台任务测试用）
}

}  // namespace

// ============================================================
// 元信息
// ============================================================

TEST_CASE("BashTool name is Bash", "[tool][bash][metadata]") {
    BashTool tool;
    REQUIRE(tool.name() == "Bash");
}

TEST_CASE("BashTool description non-empty", "[tool][bash][metadata]") {
    BashTool tool;
    REQUIRE_FALSE(tool.description().empty());
}

TEST_CASE("BashTool prompt non-empty", "[tool][bash][metadata]") {
    BashTool tool;
    REQUIRE_FALSE(tool.prompt().empty());
}

TEST_CASE("BashTool input_schema has command required", "[tool][bash][metadata]") {
    BashTool tool;
    auto schema = tool.input_schema();
    REQUIRE(schema["type"] == "object");
    REQUIRE(schema["properties"].contains("command"));
    // required 是数组，检查元素是否存在
    bool command_required = false;
    for (const auto& item : schema["required"]) {
        if (item.get<std::string>() == "command") {
            command_required = true;
            break;
        }
    }
    REQUIRE(command_required);
    // 验证新增字段
    REQUIRE(schema["properties"].contains("run_in_background"));
    REQUIRE(schema["properties"].contains("dangerously_disable_sandbox"));
    REQUIRE(schema["properties"].contains("timeout"));
    REQUIRE(schema["properties"].contains("description"));
    // M-2 修复：cwd 字段应在 schema 中声明
    REQUIRE(schema["properties"].contains("cwd"));
}

// ============================================================
// 参数校验
// ============================================================

TEST_CASE("BashTool rejects missing command", "[tool][bash][validation]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    nlohmann::json input = {{"timeout", 1000}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

TEST_CASE("BashTool rejects empty command", "[tool][bash][validation]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    nlohmann::json input = {{"command", ""}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
}

TEST_CASE("BashTool rejects non-string command", "[tool][bash][validation]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    nlohmann::json input = {{"command", 123}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

// ============================================================
// 同步执行
// ============================================================

TEST_CASE("BashTool executes echo command successfully", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    nlohmann::json input = {{"command", "echo hello_world"}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    // 输出应包含 hello_world
    REQUIRE(r.value().text.find("hello_world") != std::string::npos);
    // 应包含 <stdout> 标签
    REQUIRE(r.value().text.find("<stdout>") != std::string::npos);
}

TEST_CASE("BashTool reports non-zero exit code", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    // 命令按实际检测到的 shell 语法选择：Git Bash/POSIX 用 `exit N`，
    // cmd.exe 用 `exit /b N`（BashTool 内部同样按 detect() 选 shell，两者必须一致）
    const auto& sh = shell_detect::detect();
    nlohmann::json input = {
        {"command", sh.type == shell_detect::ShellType::CmdExe ? "exit /b 42" : "exit 42"}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("42") != std::string::npos);
    REQUIRE(r.value().text.find("<error>") != std::string::npos);
}

TEST_CASE("BashTool handles timeout", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
#ifdef _WIN32
    nlohmann::json input = {{"command", "ping -n 10 127.0.0.1"}, {"timeout", 200}};
#else
    nlohmann::json input = {{"command", "sleep 10"}, {"timeout", 200}};
#endif

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("timed out") != std::string::npos);
}

TEST_CASE("BashTool respects cancellation", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);
    std::atomic<bool> cancel{true};
    ctx.cancel_flag = &cancel;

    nlohmann::json input = {{"command", "echo should_not_run"}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::Cancelled);
}

// ============================================================
// 后台任务
// ============================================================

TEST_CASE("BashTool background requires TaskManager", "[tool][bash][background]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);  // task_manager_ptr = nullptr

    nlohmann::json input = {{"command", "echo bg"}, {"run_in_background", true}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ConfigInvalid);
}

#ifndef _WIN32
TEST_CASE("BashTool background cancel kills process and marks Cancelled",
          "[tool][bash][background][cancel]") {
    // #23 P3：run_in_background 任务取消联动。
    // task->cancel() → should_cancel → subprocess 轮询 → 进程组 kill（P1 已通），
    // 任务以 Cancelled 结束但 BashTool::call 不报错（后台立即返回 task_id）。
    EventBus::instance().clear();
    auto& task_mgr = TaskManager::instance();
    task_mgr.cancelAll();
    task_mgr.waitForAll();
    task_mgr.update();

    ToolContext ctx;
    fill_ctx(ctx);
    ctx.task_manager_ptr = &task_mgr;

    BashTool tool;
    nlohmann::json input = {{"command", "sleep 30"}, {"run_in_background", true}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());

    // 从返回文本提取 task_id（格式 "ID: bash:<n>:<cmd>"）
    std::string text = r.value().text;
    const std::string prefix = "ID: ";
    REQUIRE(text.find(prefix) != std::string::npos);
    std::string id = text.substr(text.find(prefix) + prefix.size());
    id = id.substr(0, id.find('\n'));
    REQUIRE(id.rfind("bash:", 0) == 0);

    // 找到后台任务并取消
    std::shared_ptr<Task> bg_task;
    for (const auto& t : task_mgr.getTasks()) {
        if (t->getName() == id) {
            bg_task = t;
            break;
        }
    }
    REQUIRE(bg_task != nullptr);

    task_mgr.cancel(bg_task);
    task_mgr.wait(bg_task);
    REQUIRE(bg_task->getStatus() == TaskStatus::Cancelled);

    // 清理：取消全部遗留任务并清空事件队列，避免影响其他测试
    task_mgr.cancelAll();
    task_mgr.waitForAll();
    task_mgr.update();
    EventBus::instance().clear();
}
#endif  // !_WIN32

// ============================================================
// 进度回调
// ============================================================

TEST_CASE("BashTool reports progress via callback", "[tool][bash][progress]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    std::vector<std::string> progress_reports;
    ctx.progress_callback = [&progress_reports](const std::string& text) {
        progress_reports.push_back(text);
    };

    nlohmann::json input = {{"command", "echo progress_test"}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    // 应至少上报 2 次（开始 + 完成）
    REQUIRE(progress_reports.size() >= 2);
    // 第一次包含 "Executing"
    REQUIRE(progress_reports[0].find("Executing") != std::string::npos);
}

// ============================================================
// 沙盒配置
// ============================================================

TEST_CASE("BashTool accepts dangerously_disable_sandbox", "[tool][bash][sandbox]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    // 禁用沙盒不应导致错误（实际是否降级由平台决定）
    nlohmann::json input = {{"command", "echo no_sandbox"}, {"dangerously_disable_sandbox", true}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("no_sandbox") != std::string::npos);
}

// ============================================================
// 超时上限
// ============================================================

TEST_CASE("BashTool clamps timeout to max", "[tool][bash][timeout]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    // 设置超过上限的 timeout，应被截断为 600000ms
    nlohmann::json input = {{"command", "echo max_timeout"}, {"timeout", 999999999}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("max_timeout") != std::string::npos);
}

// ============================================================
// M-2 修复：cwd 字段从 input 读取
// ============================================================

TEST_CASE("BashTool accepts cwd field in input", "[tool][bash][cwd]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    // 通过 input 指定 cwd，应在该目录下执行
    std::string temp_dir = std::filesystem::current_path().string();
    nlohmann::json input = {{"command", "echo cwd_test"}, {"cwd", temp_dir}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("cwd_test") != std::string::npos);
}

// ============================================================
// L-1 修复：strip_empty_lines 末尾不应有多余换行
// ============================================================

TEST_CASE("BashTool output has no trailing blank line in stdout", "[tool][bash][output]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    // echo 输出末尾自带换行，strip_empty_lines 应只保留一个
    // 验证 </stdout> 紧跟在内容之后，不出现空行
    nlohmann::json input = {{"command", "echo line1"}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    // 不应出现 "\n</stdout>"（即 stdout 内容末尾有空行）
    REQUIRE(r.value().text.find("\n\n</stdout>") == std::string::npos);
    // 应出现 "line1\n</stdout>"
    REQUIRE(r.value().text.find("line1\n</stdout>") != std::string::npos);
}
