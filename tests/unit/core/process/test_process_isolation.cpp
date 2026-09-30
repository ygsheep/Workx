/**
 * @file test_process_isolation.cpp
 * @brief 进程级隔离单元测试（#84 方案 B）
 * @details 覆盖三层：
 *          1. 纯函数 —— SandboxAdapter::derive_isolation() 的档位语义
 *          2. 跨平台 —— 隔离规格是否被带进 WrappedCommand；未请求时
 *             ExecOutput::isolation 是否为 NotApplicable
 *          3. Windows 专属 —— Job Object 的真实行为：正常施加、超时走
 *             TerminateJobObject 连带终止孙进程、正常结束由
 *             JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 清理孤儿、进程数上限生效。
 *             超时那条附「无 job 时孙进程残留」的对照组，证明差异真实存在。
 *
 *          第 3 层在 CI 上跑不到（code-quality.yml 的 job 全是 ubuntu-latest，
 *          无 Windows runner），其通过证据需在 Windows 机器上手工取得并贴进 PR。
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>

#include "core/process/exec_output.h"
#include "core/process/process_isolation.h"
#include "core/process/sandbox/sandbox_adapter.h"
#include "core/process/sandbox/sandbox_config.h"
#include "core/process/subprocess.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace agent;
using namespace agent::process;
namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
constexpr const char* kShell = "cmd.exe";
#else
constexpr const char* kShell = "/bin/sh";
#endif

sandbox::SandboxConfig restrictive_config() { return sandbox::SandboxConfig::restrictive("."); }

fs::path make_probe_dir(const std::string& tag) {
    const auto stamp = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    auto dir = fs::temp_directory_path() / ("workx_iso_" + tag + "_" + stamp);
    fs::create_directories(dir);
    return dir;
}

#ifdef _WIN32

/// 探测进程是否存活
/// @details 直接走 Win32 API，而不是 `tasklist /FI "PID eq N"`：后者的过滤表达式含
///          引号，而 exec 的 escape_arg 会把它转义成 \"，cmd.exe 解析失败后恒返回空
///          输出 —— 那样本函数永远返回 false，把"孙进程已死"的断言变成永远通过的
///          假守卫（本文件的对照组正是用来抓这种事的）。
bool process_alive(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return false;
    DWORD exit_code = 0;
    const bool queried = GetExitCodeProcess(h, &exit_code) != 0;
    CloseHandle(h);
    return queried && exit_code == STILL_ACTIVE;
}

/// 强制清理对照组留下的孤儿进程
void force_kill(unsigned long pid) {
    ExecOptions opts;
    opts.args = {"/c", "taskkill /F /PID " + std::to_string(pid)};
    (void)exec("cmd.exe", opts);
}

/// 孙进程脚本：写出自己的 PID 后长睡
/// @details 写成脚本文件而非内联命令：escape_arg 按 CommandLineToArgvW 规则转义，
///          而 cmd.exe 并不按该规则解析命令行，内联的 \" 会被它当成字面反斜杠。
void write_child_script(const fs::path& dir) {
    std::ofstream ps_file(dir / "child.ps1");
    ps_file << "$PID | Out-File -Encoding ascii 'child.pid'\r\n"
            << "Start-Sleep 30\r\n";
}

/// 超时路径探针：派生孙进程后自己长挂，等 exec 的超时把它连树带走
void write_timeout_probe(const fs::path& dir) {
    write_child_script(dir);
    std::ofstream f(dir / "probe.cmd");
    f << "@echo off\r\n"
      << "start \"\" /b powershell -NoProfile -ExecutionPolicy Bypass -File child.ps1\r\n"
      << "ping -n 30 127.0.0.1 > nul\r\n";
}

/// 正常结束路径探针：派生孙进程后**自己先退出**，把"清理孤儿"交给
/// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE（关闭 job 句柄时生效）
/// @details 退出前轮询 pid 文件，确保孙进程确实已经起来 —— 否则 cmd 可能在
///          powershell 完成进程创建前就跑完，用例会变成在测一条空路径。
void write_exit_probe(const fs::path& dir) {
    write_child_script(dir);
    std::ofstream f(dir / "probe.cmd");
    f << "@echo off\r\n"
      << "start \"\" /b powershell -NoProfile -ExecutionPolicy Bypass -File child.ps1\r\n"
      << "set /a tries=0\r\n"
      << ":wait\r\n"
      << "if exist child.pid exit /b 0\r\n"
      << "set /a tries+=1\r\n"
      << "if %tries% geq 20 exit /b 1\r\n"
      << "ping -n 2 127.0.0.1 > nul\r\n"
      << "goto wait\r\n";
}

/// 等孙进程把 pid 写出来
unsigned long wait_for_pid(const fs::path& pid_file, int attempts) {
    for (int i = 0; i < attempts; ++i) {
        if (fs::exists(pid_file)) {
            std::ifstream ifs(pid_file);
            unsigned long pid = 0;
            if (ifs >> pid) return pid;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return 0;
}

/// 跑一次探针，返回孙进程 pid（0 表示脚本没来得及写出 pid 文件）
unsigned long run_probe(const fs::path& dir, std::optional<ProcessIsolationSpec> spec,
                        std::chrono::milliseconds timeout, bool* timed_out) {
    ExecOptions opts;
    opts.cwd = dir.string();
    opts.args = {"/c", "probe.cmd"};
    opts.timeout = timeout;
    opts.isolation = std::move(spec);

    const auto r = exec("cmd.exe", opts);
    if (r.is_ok()) *timed_out = r.value().timed_out;
    return wait_for_pid(dir / "child.pid", 60);
}

#endif  // _WIN32

}  // namespace

// ============================================================
// 纯函数：档位语义
// ============================================================

TEST_CASE("restrictive 配置派生出进程树与资源约束", "[process_isolation][issue84]") {
    const auto spec = sandbox::SandboxAdapter::derive_isolation(restrictive_config());
    REQUIRE(spec.has_value());
    REQUIRE(spec->max_processes > 0);
    REQUIRE(spec->max_process_memory_bytes > 0);
    REQUIRE(spec->kill_tree_on_exit);
    REQUIRE(spec->is_meaningful());
}

TEST_CASE("permissive 配置不施加任何进程级约束", "[process_isolation][issue84]") {
    const auto spec =
        sandbox::SandboxAdapter::derive_isolation(sandbox::SandboxConfig::permissive());
    REQUIRE_FALSE(spec.has_value());
}

TEST_CASE("空规格不算有意义的约束", "[process_isolation][issue84]") {
    ProcessIsolationSpec spec;
    spec.kill_tree_on_exit = false;
    REQUIRE_FALSE(spec.is_meaningful());

    spec.max_processes = 1;
    REQUIRE(spec.is_meaningful());
}

// ============================================================
// 跨平台：规格的传递与结果的默认值
// ============================================================

TEST_CASE("宽松配置经 wrap_command 后不带隔离规格", "[process_isolation][issue84]") {
    const auto lax = sandbox::SandboxAdapter::wrap_command(kShell, {"-c", "true"},
                                                           sandbox::SandboxConfig::permissive());
    REQUIRE_FALSE(lax.isolation.has_value());
}

TEST_CASE("未请求隔离时结果为 NotApplicable", "[process_isolation][issue84]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "echo ok"};
#else
    opts.args = {"-c", "echo ok"};
#endif
    const auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
    REQUIRE(r.value().isolation == IsolationOutcome::NotApplicable);
}

#ifndef _WIN32
TEST_CASE("POSIX 的进程组机制接管后不重复施加 job 约束", "[process_isolation][issue84]") {
    // exec_posix 已用 setpgid + kill(-pid) 覆盖进程树终止，故 isolation 请求被忽略，
    // 结果保持 NotApplicable —— 这是"不需要第二个机制"的显式表达，不是遗漏。
    ExecOptions opts;
    opts.args = {"-c", "echo ok"};
    opts.isolation = sandbox::SandboxAdapter::derive_isolation(restrictive_config());
    const auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
    REQUIRE(r.value().isolation == IsolationOutcome::NotApplicable);
}
#endif

// ============================================================
// Windows：Job Object 的真实行为
// ============================================================

#ifdef _WIN32

TEST_CASE("Windows 严格档经 wrap_command 带上隔离规格", "[process_isolation][issue84][windows]") {
    const auto wrapped =
        sandbox::SandboxAdapter::wrap_command("cmd.exe", {"/c", "echo"}, restrictive_config());
    // 无 FS/网络后端 → degraded；但进程级约束仍在（#84 方案 B 的核心语义）
    REQUIRE(wrapped.degraded);
    REQUIRE(wrapped.isolation.has_value());
    REQUIRE(wrapped.isolation->kill_tree_on_exit);
}

TEST_CASE("Windows 上请求隔离时 job object 真实生效", "[process_isolation][issue84][windows]") {
    ExecOptions opts;
    opts.args = {"/c", "echo isolated"};
    opts.isolation = sandbox::SandboxAdapter::derive_isolation(restrictive_config());
    REQUIRE(opts.isolation.has_value());

    const auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
    REQUIRE(r.value().isolation == IsolationOutcome::Applied);
    REQUIRE(r.value().stdout_text.find("isolated") != std::string::npos);
}

TEST_CASE("超时后孙进程被连带终止，无 job 时则残留成孤儿",
          "[process_isolation][issue84][windows]") {
    const auto with_dir = make_probe_dir("with");
    const auto without_dir = make_probe_dir("without");
    write_timeout_probe(with_dir);
    write_timeout_probe(without_dir);

    // --- 有 job：超时走 TerminateJobObject，整棵树一起走 ---
    bool timed_out = false;
    const auto spec = sandbox::SandboxAdapter::derive_isolation(restrictive_config());
    const unsigned long killed_pid =
        run_probe(with_dir, spec, std::chrono::milliseconds(4000), &timed_out);
    REQUIRE(timed_out);
    REQUIRE(killed_pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE_FALSE(process_alive(killed_pid));

    // --- 无 job（对照）：TerminateProcess 只杀直接子进程，孙进程存活 ---
    bool timed_out_ref = false;
    const unsigned long survivor_pid =
        run_probe(without_dir, std::nullopt, std::chrono::milliseconds(4000), &timed_out_ref);
    REQUIRE(timed_out_ref);
    REQUIRE(survivor_pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE(process_alive(survivor_pid));
    force_kill(survivor_pid);

    std::error_code ec;
    fs::remove_all(with_dir, ec);
    fs::remove_all(without_dir, ec);
}

TEST_CASE("超时连带终止不依赖 KILL_ON_JOB_CLOSE", "[process_isolation][issue84][windows]") {
    // 关掉 KILL_ON_JOB_CLOSE，但留一个进程数上限（否则 job 规格为空、根本不会建立）。
    // 此时若超时走 TerminateProcess 而不是 TerminateJobObject，孙进程就会残留 ——
    // 这条用例专门守住 terminate_child() 的 job 分支：在默认档下它被
    // KILL_ON_JOB_CLOSE 兜住，换成 TerminateProcess 也看不出差别（等价变异）。
    ProcessIsolationSpec spec;
    spec.max_processes = 64;
    spec.kill_tree_on_exit = false;

    const auto dir = make_probe_dir("no_killclose");
    write_timeout_probe(dir);

    bool timed_out = false;
    const unsigned long orphan_pid =
        run_probe(dir, spec, std::chrono::milliseconds(4000), &timed_out);
    REQUIRE(timed_out);
    REQUIRE(orphan_pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE_FALSE(process_alive(orphan_pid));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("命令正常结束后孤儿孙进程仍被清理", "[process_isolation][issue84][windows]") {
    // 覆盖 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE：直接子进程（cmd）自己跑完退出，
    // 但它 start /b 派生的孙进程还在跑 —— 关闭 job 句柄时应把它一并带走。
    // 这条路径与"超时"不同（那里靠 TerminateJobObject），缺了它就没有用例守住该标志位。
    const auto dir = make_probe_dir("exited");
    write_exit_probe(dir);

    bool timed_out = false;
    const auto spec = sandbox::SandboxAdapter::derive_isolation(restrictive_config());
    const unsigned long orphan_pid =
        run_probe(dir, spec, std::chrono::milliseconds(60000), &timed_out);
    REQUIRE_FALSE(timed_out);
    REQUIRE(orphan_pid > 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    REQUIRE_FALSE(process_alive(orphan_pid));

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("进程数上限生效：超出上限的派生被拒绝", "[process_isolation][issue84][windows]") {
    const auto dir = make_probe_dir("proclimit");
    {
        std::ofstream f(dir / "spawn.ps1");
        f << "$started = 0\r\n"
          << "1..4 | ForEach-Object {\r\n"
          << "  try { Start-Process cmd -ArgumentList '/c','ping -n 8 127.0.0.1 > nul'"
             " -WindowStyle Hidden -ErrorAction Stop; $started++ } catch { }\r\n"
          << "}\r\n"
          << "Write-Output \"STARTED=$started\"\r\n";
    }

    ProcessIsolationSpec spec;
    spec.max_processes = 3;  // cmd 与 powershell 已占两席，只容得下一次派生
    spec.kill_tree_on_exit = true;

    ExecOptions opts;
    opts.cwd = dir.string();
    opts.args = {"/c", "powershell -NoProfile -ExecutionPolicy Bypass -File spawn.ps1"};
    opts.isolation = spec;
    opts.timeout = std::chrono::milliseconds(30000);

    const auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().isolation == IsolationOutcome::Applied);
    // 上限没生效的话四个都会成功；只要少于四个，就说明 job 拦住了派生
    REQUIRE(r.value().stdout_text.find("STARTED=4") == std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

#endif  // _WIN32
