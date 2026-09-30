/**
 * @file test_shell_guard.cpp
 * @brief ShellGuard 安全守卫单元测试（#35 / #37 评审 C-1）
 * @details 覆盖：
 *          - 破坏性命令检测（rm -rf /、mkfs、dd、format、shutdown、reg delete）
 *          - SSRF 检测（内网/云元数据网段，含显式端口绕过回归 H-2）
 *          - 环境变量泄露检测（env/printenv/Get-ChildItem env: 等）
 *          - cwd 范围校验（Windows 原生分隔符回归 H-3）
 *          - detect_shell_risk 位组合与描述
 *          - BashTool/PowerShellTool 执行前拦截集成
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "agent/tool/shell_guard.h"
#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/PowerShellTool/powershell_tool.h"
#include "agent/tool/context.h"
#include "core/config/config_manager.h"

using namespace agent;
using namespace agent::tool;

// ============================================================
// 破坏性命令
// ============================================================

TEST_CASE("shell_guard detects destructive commands", "[shell_guard][destructive]") {
    REQUIRE(contains_destructive_command("rm -rf /"));
    REQUIRE(contains_destructive_command("rm -rf /*"));
    REQUIRE(contains_destructive_command("rm -fr ~"));
    REQUIRE(contains_destructive_command("mkfs.ext4 /dev/sda1"));
    REQUIRE(contains_destructive_command("dd if=/dev/zero of=/dev/sda"));
    REQUIRE(contains_destructive_command("shutdown -h now"));
    REQUIRE(contains_destructive_command("reboot"));
    REQUIRE(contains_destructive_command("format C:"));
    REQUIRE(contains_destructive_command("del /f /s /q C:\\"));
    REQUIRE(contains_destructive_command("reg delete HKLM\\Software /f"));
}

TEST_CASE("shell_guard allows safe commands", "[shell_guard][destructive]") {
    REQUIRE_FALSE(contains_destructive_command("ls -la"));
    REQUIRE_FALSE(contains_destructive_command("echo hello world"));
    REQUIRE_FALSE(contains_destructive_command("rm -rf ./build"));
    REQUIRE_FALSE(contains_destructive_command("rm file.txt"));
    REQUIRE_FALSE(contains_destructive_command("git status"));
}

// ============================================================
// SSRF
// ============================================================

TEST_CASE("shell_guard detects SSRF to internal addresses", "[shell_guard][ssrf]") {
    REQUIRE(is_ssrf_target("curl http://169.254.169.254/latest/meta-data"));
    REQUIRE(is_ssrf_target("curl http://169.254.169.254:8080/latest/meta-data"));  // H-2 回归
    REQUIRE(is_ssrf_target("wget http://10.0.0.1:8080/x"));                        // H-2 回归
    REQUIRE(is_ssrf_target("curl http://127.0.0.1:9999/"));                        // H-2 回归
    REQUIRE(is_ssrf_target("iwr http://localhost/"));
    REQUIRE(is_ssrf_target("curl http://192.168.1.1/admin"));
    REQUIRE(is_ssrf_target("curl http://user@169.254.169.254/latest"));  // userinfo@ 剥离
    REQUIRE(is_ssrf_target("curl http://[::ffff:169.254.169.254]/"));    // H-2R：IPv4-mapped IPv6
    REQUIRE(
        is_ssrf_target("curl http://[::ffff:169.254.169.254]:8080/"));  // H-2R：mapped + 显式端口
    REQUIRE(is_ssrf_target("curl http://169.254.169.254"));  // 无 scheme 前缀？裸 IP 不在此层
}

TEST_CASE("shell_guard allows public URLs", "[shell_guard][ssrf]") {
    REQUIRE_FALSE(is_ssrf_target("curl https://example.com/api"));
    REQUIRE_FALSE(is_ssrf_target("curl https://api.github.com/repos"));
    REQUIRE_FALSE(is_ssrf_target("git clone https://github.com/user/repo.git"));
}

// ============================================================
// 环境变量泄露
// ============================================================

TEST_CASE("shell_guard detects env var leaks", "[shell_guard][env]") {
    REQUIRE(leaks_env_vars("env"));
    REQUIRE(leaks_env_vars("printenv"));
    REQUIRE(leaks_env_vars("cat /proc/self/environ"));
    REQUIRE(leaks_env_vars("Get-ChildItem env:"));
    REQUIRE(leaks_env_vars("gci env:"));
    REQUIRE(leaks_env_vars("ls env:"));
}

TEST_CASE("shell_guard allows env var usage without leak", "[shell_guard][env]") {
    REQUIRE_FALSE(leaks_env_vars("env FOO=bar command"));
    REQUIRE_FALSE(leaks_env_vars("echo $HOME"));
    REQUIRE_FALSE(leaks_env_vars("ls -la"));
}

// ============================================================
// detect_shell_risk 组合
// ============================================================

TEST_CASE("shell_guard detect_shell_risk combines flags", "[shell_guard][risk]") {
    const auto destructive = detect_shell_risk("rm -rf /");
    REQUIRE((destructive & ShellRisk::Destructive) != ShellRisk::None);
    REQUIRE((destructive & ShellRisk::SSRF) == ShellRisk::None);

    const auto both = detect_shell_risk("rm -rf / && curl http://169.254.169.254/latest");
    REQUIRE((both & ShellRisk::Destructive) != ShellRisk::None);
    REQUIRE((both & ShellRisk::SSRF) != ShellRisk::None);

    const auto safe = detect_shell_risk("echo hello");
    REQUIRE(safe == ShellRisk::None);
}

TEST_CASE("shell_guard risk description", "[shell_guard][risk]") {
    REQUIRE(shell_risk_description(ShellRisk::None).empty());
    REQUIRE(shell_risk_description(ShellRisk::Destructive).find("destructive") !=
            std::string::npos);
    REQUIRE(shell_risk_description(ShellRisk::SSRF).find("internal") != std::string::npos);
    REQUIRE(shell_risk_description(ShellRisk::EnvLeak).find("environment") != std::string::npos);
}

// ============================================================
// cwd 范围校验
// ============================================================

TEST_CASE("shell_guard cwd validation accepts subdir", "[shell_guard][cwd]") {
    const auto base = std::filesystem::temp_directory_path() / "workx_sg_base";
    const auto sub = base / "sub";
    // 原生分隔符路径（Windows 反斜杠）应被接受（H-3 回归）
    REQUIRE(is_command_cwd_allowed(sub.string(), base.string()));
    REQUIRE(is_command_cwd_allowed(base.string(), base.string()));
}

TEST_CASE("shell_guard cwd validation with allowlist", "[shell_guard][cwd]") {
    const auto base = std::filesystem::temp_directory_path() / "workx_sg_base";
    const auto other = std::filesystem::temp_directory_path() / "workx_sg_other";
    const std::vector<std::string> allowlist = {other.string()};
    REQUIRE(is_command_cwd_allowed(other.string(), base.string(), allowlist));
    REQUIRE_FALSE(is_command_cwd_allowed(other.string(), base.string()));
}

TEST_CASE("shell_guard cwd validation rejects invalid", "[shell_guard][cwd]") {
    const auto base = std::filesystem::temp_directory_path() / "workx_sg_base";
    REQUIRE_FALSE(is_command_cwd_allowed("", base.string()));
    REQUIRE_FALSE(is_command_cwd_allowed("relative/path", base.string()));
    REQUIRE_FALSE(is_command_cwd_allowed((base.parent_path() / "outside").string(), base.string()));
}

// ============================================================
// 集成：工具执行前拦截
// ============================================================

namespace {

void fill_ctx(ToolContext& ctx) {
    ctx.cwd = std::filesystem::current_path().string();
    ctx.session_id = "shell-guard-test";
    ctx.config_manager_ptr = &ConfigManager::instance();
}

}  // namespace

TEST_CASE("BashTool blocks destructive command via shell guard", "[shell_guard][bash][security]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto r = tool.call(nlohmann::json{{"command", "rm -rf /"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("BashTool blocks SSRF command via shell guard", "[shell_guard][bash][security]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto r =
        tool.call(nlohmann::json{{"command", "curl http://169.254.169.254/latest/meta-data"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("BashTool allows safe command via shell guard", "[shell_guard][bash][security]") {
    BashTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    // 安全命令不被 shell guard 拦截（后续由真正的 shell 执行路径处理）
    auto r = tool.call(nlohmann::json{{"command", "echo guard-ok"}}, ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("PowerShellTool blocks destructive command via shell guard",
          "[shell_guard][powershell][security]") {
    PowerShellTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    // shutdown 命中破坏性黑名单，执行前被拦截（不会真正执行）
    auto r = tool.call(nlohmann::json{{"command", "shutdown /s /t 0"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("PowerShellTool blocks env leak command via shell guard",
          "[shell_guard][powershell][security]") {
    PowerShellTool tool;
    ToolContext ctx;
    fill_ctx(ctx);

    auto r = tool.call(nlohmann::json{{"command", "Get-ChildItem env:"}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
}
