/**
 * @file test_permission_security.cpp
 * @brief #36 权限决策层单元测试
 * @details 覆盖：
 *          - PermissionMode 模式判定纯函数（is_plan_mode/is_bypass_mode/deny_*）
 *          - 危险命令检测（is_dangerous_command）
 *          - ask_user_confirm fail-closed（无 event_bus → 拒绝）
 *          - FileWriteTool：Plan 拒写 / Bypass 放行 / 密钥内容拒绝 / 越界路径拒绝
 *          - FileReadTool：越界路径拒绝 / 敏感路径拒绝 / 输出脱敏
 *          - BashTool：Plan 拒执行 / 危险命令需确认（无宿主 → 拒绝） / 安全命令放行
 */

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/GlobTool/glob_tool.h"
#include "agent/tool/GrepTool/grep_tool.h"
#include "agent/tool/context.h"
#include "agent/tool/path_validator.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/secret_scanner.h"
#include "core/config/config_manager.h"
#include "core/events/agent_events.h"
#include "core/utils/error.h"
#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::tool;
namespace fs = std::filesystem;

namespace {

/// 最小可用 ToolContext（注入 ConfigManager 单例）
void fill_ctx(ToolContext& ctx, const fs::path& cwd) {
    ctx.cwd = cwd.string();
    ctx.session_id = "test";
    ctx.config_manager_ptr = &ConfigManager::instance();
    ctx.event_bus_ptr = nullptr;  // 无宿主确认通道
}

/// 临时目录夹具：RAII 创建/清理
struct TempDir {
    fs::path path;
    TempDir()
        : path(fs::temp_directory_path() /
               ("workx_perm_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "_" +
                std::to_string(std::random_device{}()))) {
        fs::create_directories(path);
    }
    ~TempDir() { fs::remove_all(path); }
};

}  // namespace

// ============================================================
// 模式判定纯函数
// ============================================================

TEST_CASE("PermissionMode helpers", "[tool][permission]") {
    REQUIRE_FALSE(is_plan_mode(PermissionMode::Default));
    REQUIRE(is_plan_mode(PermissionMode::Plan));
    REQUIRE_FALSE(is_bypass_mode(PermissionMode::Plan));
    REQUIRE(is_bypass_mode(PermissionMode::BypassPermissions));

    REQUIRE_FALSE(deny_write_by_mode(PermissionMode::Default));
    REQUIRE(deny_write_by_mode(PermissionMode::Plan));
    REQUIRE_FALSE(deny_write_by_mode(PermissionMode::BypassPermissions));

    REQUIRE_FALSE(deny_execute_by_mode(PermissionMode::Default));
    REQUIRE(deny_execute_by_mode(PermissionMode::Plan));
    REQUIRE_FALSE(deny_execute_by_mode(PermissionMode::BypassPermissions));
}

// ============================================================
// 危险命令检测
// ============================================================

TEST_CASE("is_dangerous_command detects destructive patterns", "[tool][permission]") {
    REQUIRE(is_dangerous_command("rm -rf /tmp/foo"));
    REQUIRE(is_dangerous_command("sudo rm -r /"));
    REQUIRE(is_dangerous_command("shutdown -r now"));
    REQUIRE(is_dangerous_command("cat x | bash"));
    REQUIRE(is_dangerous_command("echo hi | powershell -c x"));
    REQUIRE(is_dangerous_command("mkfs.ext4 /dev/sda1"));
}

TEST_CASE("is_dangerous_command allows safe commands", "[tool][permission]") {
    REQUIRE_FALSE(is_dangerous_command("echo hello"));
    REQUIRE_FALSE(is_dangerous_command("git status"));
    REQUIRE_FALSE(is_dangerous_command("ls -la"));
    REQUIRE_FALSE(is_dangerous_command("rm file.txt"));  // 无 -rf/-r / 等破坏性组合
}

TEST_CASE("is_dangerous_command detects disk format only with drive/switch",
          "[tool][permission][review]") {
    // 评审 #6：format 需为独立命令（盘符/参数），避免误伤 printf("format %d")
    REQUIRE(is_dangerous_command("format C:"));
    REQUIRE(is_dangerous_command("format /Q"));
    REQUIRE_FALSE(is_dangerous_command("printf(\"format %d\", x)"));
    REQUIRE_FALSE(is_dangerous_command("echo format text"));
}

// ============================================================
// 评审 #2：绝对禁止 vs 可确认敏感路径
// ============================================================

TEST_CASE("is_absolutely_forbidden_path hard-rejects keys/credentials",
          "[tool][permission][review]") {
    REQUIRE(is_absolutely_forbidden_path("/home/user/.ssh/id_rsa"));
    REQUIRE(is_absolutely_forbidden_path("/home/user/.ssh/id_ed25519"));
    REQUIRE(is_absolutely_forbidden_path("/home/user/credentials"));
    REQUIRE(is_absolutely_forbidden_path("/home/user/.git-credentials"));
    REQUIRE(is_absolutely_forbidden_path("C:\\Users\\x\\.ssh\\id_rsa"));
    // 可确认敏感（.env、配置文件、普通 hooks）不算绝对禁止
    REQUIRE_FALSE(is_absolutely_forbidden_path("/project/.env"));
    REQUIRE_FALSE(is_absolutely_forbidden_path("/project/src/config/app.cpp"));
    REQUIRE_FALSE(is_absolutely_forbidden_path("/project/.git/hooks/pre-commit"));
}

// ============================================================
// 路径边界：分隔符归一化 + git 仓库根放行
// ============================================================

TEST_CASE("is_within_allowed_root handles mixed path separators", "[tool][permission][path]") {
    TempDir tmp;
    fs::path root = tmp.path / "root";
    fs::create_directories(root);
    fs::path inside = root / "in.txt";
    fs::path outside = tmp.path / "out.txt";  // root 的兄弟目录（越界）
    {
        std::ofstream ofs(inside);
        ofs << "x";
    }
    {
        std::ofstream ofs(outside);
        ofs << "x";
    }

    // cwd 用平台原生分隔符（Windows 反斜杠），canonical 用正斜杠：
    // 前缀比较前必须归一化分隔符，否则 cwd 内路径被误判越界。
    const std::string cwd = root.string();
    REQUIRE(is_within_allowed_root(fs::canonical(inside).generic_string(), cwd, {}));
    REQUIRE_FALSE(is_within_allowed_root(fs::canonical(outside).generic_string(), cwd, {}));
}

TEST_CASE("repo_root_allowlist permits git repo root and subprojects", "[tool][permission][path]") {
    TempDir tmp;
    fs::path repo = tmp.path / "repo";
    fs::create_directories(repo);
    fs::path deep = repo / "build" / "bin" / "debug";  // 深层子目录（如 build/bin/Debug）
    fs::create_directories(deep);
    fs::path file = repo / "src" / "main.cpp";  // 仓库内但不在 cwd 下
    fs::create_directories(file.parent_path());
    {
        std::ofstream ofs(file);
        ofs << "x";
    }

    // cwd 位于仓库深层子目录时，仓库根加入 allowlist 后仓库内路径可访问
    const std::string cwd = deep.string();
    const auto allowlist = repo_root_allowlist(repo.string());
    REQUIRE(is_within_allowed_root(fs::canonical(file).generic_string(), cwd, allowlist));
    REQUIRE_FALSE(is_within_allowed_root(fs::canonical(file).generic_string(), cwd, {}));
}

// ============================================================
// ask_user_confirm fail-closed
// ============================================================

TEST_CASE("ask_user_confirm fails closed without event bus", "[tool][permission]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    REQUIRE_FALSE(ask_user_confirm(ctx, "Allow?"));
}

TEST_CASE("ask_user_confirm publishes object-format questions contract", "[tool][permission]") {
    agent::test::MockEventBus bus;
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);

    nlohmann::json captured;
    bool captured_ok = false;
    bus.subscribe<agent::AskUserRequestEvent>([&](const agent::AskUserRequestEvent& e) {
        captured = e.questions;
        captured_ok = true;
        agent::AskUserResult r;
        r.submitted = true;
        r.answers.emplace_back("Allow?", "Yes");
        e.result_promise->set_value(std::move(r));
    });

    ToolContext ctx;
    ctx.session_id = "test";
    ctx.config_manager_ptr = &ConfigManager::instance();
    ctx.event_bus_ptr = &bus;

    REQUIRE(ask_user_confirm(ctx, "Allow?"));

    // 契约：questions 字段为 {questions:[...]} 对象（与 AskUserTool / parse_choice_config 一致），
    // 保证 ftxtui handle_ask_user 能解析出有效问题，避免空向量越界崩溃。
    REQUIRE(captured_ok);
    REQUIRE(captured.is_object());
    REQUIRE(captured.contains("questions"));
    REQUIRE(captured["questions"].is_array());
    REQUIRE(captured["questions"].size() == 1);
    const auto& q = captured["questions"][0];
    REQUIRE(q.contains("question"));
    REQUIRE(q["question"].get<std::string>() == "Allow?");
    REQUIRE(q.contains("header"));
    REQUIRE(q.contains("options"));
    REQUIRE(q["options"].is_array());
    REQUIRE(q["options"].size() == 2);
    // 契约：options 必须为 {label, description} 对象数组（ftxtui handle_ask_user
    // 只解析对象选项；字符串数组会被当作无选项而静默取消，权限确认永不弹出）。
    for (const auto& o : q["options"]) {
        REQUIRE(o.is_object());
        REQUIRE(o.contains("label"));
    }
    REQUIRE(q["options"][0]["label"].get<std::string>() == "Yes");
    REQUIRE(q["options"][1]["label"].get<std::string>() == "No");
    // 权限确认不需要自定义输入，避免误选
    REQUIRE(q.value("allow_custom_input", true) == false);
}

// ============================================================
// FileWriteTool
// ============================================================

TEST_CASE("FileWriteTool plan mode denies write", "[tool][permission][write]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::Plan;

    FileWriteTool tool;
    auto perm = tool.check_permissions(R"({"file_path": "out.txt", "content": "x"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("FileWriteTool bypass mode allows write", "[tool][permission][write]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::BypassPermissions;

    FileWriteTool tool;
    auto perm = tool.check_permissions(R"({"file_path": "out.txt", "content": "x"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

TEST_CASE("FileWriteTool rejects path escaping cwd", "[tool][permission][write]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    FileWriteTool tool;
    auto perm = tool.check_permissions(R"({"file_path": ".."})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("FileWriteTool validates secret content", "[tool][permission][write]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    FileWriteTool tool;
    auto res = tool.validate_input(
        R"({"file_path": "out.txt", "content": "token: ghp_0123456789abcdefghijklmnopqrstuvwxyzABCD"})"_json,
        ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::PermissionDenied);
}

// ============================================================
// FileReadTool
// ============================================================

TEST_CASE("FileReadTool rejects path escaping cwd", "[tool][permission][read]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    FileReadTool tool;
    auto perm = tool.check_permissions(R"({"file_path": "../secret.txt"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("FileReadTool rejects sensitive path", "[tool][permission][read]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    FileReadTool tool;
    auto perm = tool.check_permissions(R"({"file_path": ".env"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("FileReadTool redacts secrets from output", "[tool][permission][read]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    // 在 cwd 内建一个含密钥的文件
    fs::path target = tmp.path / "conf.txt";
    {
        std::ofstream ofs(target, std::ios::binary);
        ofs << "api_key = sk-ant-api03-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\n";
        ofs << "safe = hello\n";
    }
    // 模拟 pre-read 状态（脱敏测试不依赖写入，但 Read 正常路径会记录状态）
    (void)ctx;

    FileReadTool tool;
    auto res = tool.call(R"({"file_path": "conf.txt"})"_json, ctx);
    REQUIRE(res.is_ok());
    const std::string out = res.value().text;
    REQUIRE(out.find("sk-ant-api03-abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN") == std::string::npos);
    REQUIRE(out.find("[REDACTED:Anthropic API Key]") != std::string::npos);
}

TEST_CASE("FileReadTool rejects image files with no-image-input error",
          "[tool][permission][read]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    fs::path target = tmp.path / "photo.png";
    {
        std::ofstream ofs(target, std::ios::binary);
        ofs << "\x89PNG\r\n\x1a\n" << std::string(32, '\x00');
    }

    FileReadTool tool;
    auto res = tool.call(R"({"file_path": "photo.png"})"_json, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::InvalidInput);
    const std::string msg = res.error().message;
    REQUIRE(msg.find("image") != std::string::npos);
    REQUIRE(msg.find("no image input support") != std::string::npos);
    REQUIRE(msg.find("photo.png") != std::string::npos);
}

// ============================================================
// BashTool
// ============================================================

TEST_CASE("BashTool plan mode denies execution", "[tool][permission][bash]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::Plan;

    BashTool tool;
    auto perm = tool.check_permissions(R"({"command": "echo hi"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("BashTool dangerous command requires confirmation", "[tool][permission][bash]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::Default;

    BashTool tool;
    // 无宿主确认通道（event_bus_ptr == nullptr）→ fail-closed 拒绝
    auto perm = tool.check_permissions(R"({"command": "rm -rf /tmp/x"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("BashTool safe command allowed in default mode", "[tool][permission][bash]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::Default;

    BashTool tool;
    auto perm = tool.check_permissions(R"({"command": "echo hello"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

// ============================================================
// GrepTool — #60：路径边界校验
// ============================================================

TEST_CASE("GrepTool rejects path escaping cwd", "[tool][permission][grep]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GrepTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "foo", "path": "../secret"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("GrepTool rejects sensitive path", "[tool][permission][grep]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GrepTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "foo", "path": ".env"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("GrepTool allows path within cwd", "[tool][permission][grep]") {
    TempDir tmp;
    fs::path subdir = tmp.path / "src";
    fs::create_directories(subdir);
    {
        std::ofstream ofs(subdir / "a.cpp");
        ofs << "needle\n";
    }

    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GrepTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "foo", "path": "src"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

TEST_CASE("GrepTool allows default cwd path", "[tool][permission][grep]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GrepTool tool;
    // path 未指定 → 默认 ctx.cwd，无需校验
    auto perm = tool.check_permissions(R"({"pattern": "foo"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

TEST_CASE("GrepTool bypass mode allows any path", "[tool][permission][grep]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::BypassPermissions;

    GrepTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "foo", "path": "../outside"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

// ============================================================
// GlobTool — #60：路径边界校验
// ============================================================

TEST_CASE("GlobTool rejects cwd escaping boundary", "[tool][permission][glob]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GlobTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "**/*", "cwd": "../outside"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("GlobTool rejects sensitive cwd", "[tool][permission][glob]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GlobTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "**/*", "cwd": ".env"})"_json, ctx);
    REQUIRE(perm.is_err());
    REQUIRE(perm.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("GlobTool allows cwd within boundary", "[tool][permission][glob]") {
    TempDir tmp;
    fs::path subdir = tmp.path / "src";
    fs::create_directories(subdir);

    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GlobTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "**/*", "cwd": "src"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

TEST_CASE("GlobTool allows default cwd", "[tool][permission][glob]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);

    GlobTool tool;
    // cwd 未指定 → 默认 ctx.cwd，无需校验
    auto perm = tool.check_permissions(R"({"pattern": "**/*"})"_json, ctx);
    REQUIRE(perm.is_ok());
}

TEST_CASE("GlobTool bypass mode allows any cwd", "[tool][permission][glob]") {
    TempDir tmp;
    ToolContext ctx;
    fill_ctx(ctx, tmp.path);
    ctx.permission_mode = PermissionMode::BypassPermissions;

    GlobTool tool;
    auto perm = tool.check_permissions(R"({"pattern": "**/*", "cwd": "../outside"})"_json, ctx);
    REQUIRE(perm.is_ok());
}
