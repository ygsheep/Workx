/**
 * @file test_subprocess.cpp
 * @brief subprocess::exec() 跨平台单元测试
 * @details 覆盖 stdout/stderr 捕获、退出码、超时、取消、命令不存在、
 *          工作目录、参数转义、大输出截断等场景。
 *
 *          跨平台命令选择：
 *          - Windows: 使用 cmd.exe /c "整条命令"（单参数，避免 cmd 引号歧义）
 *          - POSIX:   使用 /bin/sh -c "整条命令"（确保 macOS/Linux 均可用）
 *
 *          所有测试都用 PATH 中必定存在的命令，避免依赖项目布局。
 *
 * @note cmd.exe /c 的引号规则：当参数被分别加引号时（"/c" "echo" "hello"），
 *       cmd.exe 会错误剥离引号导致命令失败。正确做法是把 /c 后的整条命令
 *       作为一个参数传入（"/c" "echo hello"），cmd.exe 会正确剥离外层引号。
 */

#include <catch2/catch_test_macros.hpp>

#include "core/process/subprocess.h"
#include "core/process/exec_output.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <unistd.h>
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

}  // namespace

// ============================================================
// 基本执行与 stdout 捕获
// ============================================================

TEST_CASE("subprocess captures stdout", "[subprocess][basic]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "echo hello"};
#else
    opts.args = {"-c", "echo hello"};
#endif
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
    REQUIRE(r.value().exit_code == 0);
    REQUIRE(r.value().stdout_text.find("hello") != std::string::npos);
    REQUIRE(r.value().stderr_text.empty());
    REQUIRE_FALSE(r.value().timed_out);
    REQUIRE_FALSE(r.value().cancelled);
}

TEST_CASE("subprocess captures stderr separately", "[subprocess][basic]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "echo err 1>&2"};
#else
    opts.args = {"-c", "echo err 1>&2"};
#endif
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().exit_code == 0);
    REQUIRE(r.value().stdout_text.empty());
    REQUIRE(r.value().stderr_text.find("err") != std::string::npos);
}

// ============================================================
// 退出码
// ============================================================

TEST_CASE("subprocess propagates non-zero exit code", "[subprocess][exit_code]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "exit 42"};
#else
    opts.args = {"-c", "exit 42"};
#endif
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE_FALSE(r.value().is_success());
    REQUIRE(r.value().exit_code == 42);
    REQUIRE_FALSE(r.value().timed_out);
    REQUIRE_FALSE(r.value().cancelled);
}

// ============================================================
// 命令不存在
// ============================================================

TEST_CASE("subprocess returns err when command not found", "[subprocess][error]") {
    ExecOptions opts;
    auto r = exec("this_command_does_not_exist_xyz_12345", opts);
    REQUIRE(r.is_err());
    // 启动失败应映射到 ResourceNotFound
    REQUIRE(r.error().code == Error::Code::ResourceNotFound);
}

// ============================================================
// 工作目录
// ============================================================

TEST_CASE("subprocess respects cwd", "[subprocess][cwd]") {
    // 创建临时目录并在其中创建一个文件，子进程列出该文件
    auto tmp_dir = fs::temp_directory_path() / "workx_subprocess_cwd_test";
    fs::create_directories(tmp_dir);
    const std::string marker = "unique_marker_file_xyz.txt";
    auto marker_path = tmp_dir / marker;
    {
        std::ofstream ofs(marker_path);
        ofs << "marker";
    }

    ExecOptions opts;
    opts.cwd = tmp_dir.string();
#ifdef _WIN32
    opts.args = {"/c", "dir /b"};
#else
    opts.args = {"-c", "ls -1"};
#endif
    auto r = exec(kShell, opts);

    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
    REQUIRE(r.value().stdout_text.find(marker) != std::string::npos);

    // 清理
    std::error_code ec;
    fs::remove(marker_path, ec);
    fs::remove(tmp_dir, ec);
}

// ============================================================
// 超时
// ============================================================

TEST_CASE("subprocess terminates on timeout", "[subprocess][timeout]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "ping -n 10 127.0.0.1"};
#else
    opts.args = {"-c", "sleep 10"};
#endif
    opts.timeout = std::chrono::milliseconds(200);
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().timed_out);
    REQUIRE_FALSE(r.value().cancelled);
}

// ============================================================
// 取消
// ============================================================

TEST_CASE("subprocess cancels via is_cancelled callback", "[subprocess][cancel]") {
    std::atomic<bool> cancel_flag{false};
    // 启动一个会触发取消的线程
    std::thread canceler([&cancel_flag]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancel_flag = true;
    });

    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "ping -n 10 127.0.0.1"};
#else
    opts.args = {"-c", "sleep 10"};
#endif
    opts.is_cancelled = [&cancel_flag]() { return cancel_flag.load(); };
    auto r = exec(kShell, opts);

    canceler.join();
    REQUIRE(r.is_ok());
    REQUIRE(r.value().cancelled);
    REQUIRE_FALSE(r.value().timed_out);
}

#ifndef _WIN32
TEST_CASE("subprocess cancel kills descendant processes via process group",
          "[subprocess][cancel][posix]") {
    // #23 P1：取消应杀整个进程组（setpgid + kill(-pid)），
    // 而非只杀 shell 直接 pid。验证 bash -c 的子孙（sleep）也被一并销毁。
    auto pidfile = fs::temp_directory_path() / "workx_subprocess_child_pid.txt";
    std::error_code ec;
    fs::remove(pidfile, ec);

    std::atomic<bool> cancel_flag{false};
    std::thread canceler([&cancel_flag]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        cancel_flag = true;
    });

    // shell 后台派生 sleep（子孙进程）并记录其 pid，再 wait 等待
    ExecOptions opts;
    opts.args = {"-c", "sleep 30 & echo $! > '" + pidfile.string() + "'; wait"};
    opts.is_cancelled = [&cancel_flag]() { return cancel_flag.load(); };
    auto r = exec(kShell, opts);
    canceler.join();

    REQUIRE(r.is_ok());
    REQUIRE(r.value().cancelled);

    // 读取子孙进程（sleep）pid
    REQUIRE(fs::exists(pidfile));
    std::ifstream ifs(pidfile);
    pid_t child_pid = 0;
    ifs >> child_pid;
    fs::remove(pidfile, ec);
    REQUIRE(child_pid > 0);

    // 等 init reaps：进程组 kill 应已让 sleep 退出并被回收，
    // kill(pid, 0) 探活应返回 ESRCH（进程不存在）
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    errno = 0;
    int alive = kill(child_pid, 0);
    REQUIRE(alive == -1);
    REQUIRE(errno == ESRCH);
}
#endif  // !_WIN32

// ============================================================
// 参数转义（多参数传递给非 shell 命令）
// ============================================================

TEST_CASE("subprocess handles multi-arg command", "[subprocess][args]") {
    ExecOptions opts;
#ifdef _WIN32
    // 用 findstr 测试多参数：echo hello | findstr hello → 输出 hello
    opts.args = {"/c", "echo hello | findstr hello"};
#else
    // 用 grep 测试多参数：echo hello | grep hello → 输出 hello
    opts.args = {"-c", "echo hello | grep hello"};
#endif
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().stdout_text.find("hello") != std::string::npos);
}

// ============================================================
// 大输出与缓冲区上限
// ============================================================

TEST_CASE("subprocess handles large output", "[subprocess][buffer]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "for /L %i in (1,1,1000) do @echo line%i"};
#else
    // sh 循环输出 1000 行（避免依赖 seq，macOS 默认无 seq）
    opts.args = {"-c", "i=1; while [ $i -le 1000 ]; do echo line$i; i=$((i+1)); done"};
#endif
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
    // 至少应该有 1000 行的内容（每行约 7 字符）
    REQUIRE(r.value().stdout_text.size() > 1000);
}

TEST_CASE("subprocess respects max_output_bytes", "[subprocess][buffer]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "for /L %i in (1,1,10000) do @echo line%i"};
#else
    opts.args = {"-c", "i=1; while [ $i -le 10000 ]; do echo line$i; i=$((i+1)); done"};
#endif
    opts.max_output_bytes = 100;
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    // 缓冲区不应显著超过上限（允许少量超出，因单次读取块）
    REQUIRE(r.value().stdout_text.size() <= 100 + 8192);
}

// ============================================================
// 边界
// ============================================================

TEST_CASE("subprocess works with minimal args", "[subprocess][basic]") {
    ExecOptions opts;
#ifdef _WIN32
    opts.args = {"/c", "echo ok"};
#else
    opts.args = {"-c", "echo ok"};
#endif
    auto r = exec(kShell, opts);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().is_success());
}

// ============================================================
// exec_interactive：交互式子进程（继承终端 stdio）
// ============================================================

TEST_CASE("exec_interactive propagates exit code", "[subprocess][interactive]") {
    // 静默命令（不输出，避免污染测试终端），验证退出码透传
#ifdef _WIN32
    auto r = exec_interactive("cmd.exe", {"/c", "exit 42"});
#else
    auto r = exec_interactive("/bin/sh", {"-c", "exit 42"});
#endif
    REQUIRE(r.is_ok());
    REQUIRE(r.value().exit_code == 42);
}

TEST_CASE("exec_interactive returns err when command not found", "[subprocess][interactive]") {
    auto r = exec_interactive("this_command_does_not_exist_xyz_12345", {});
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ResourceNotFound);
}

TEST_CASE("exec_interactive respects cwd", "[subprocess][interactive]") {
    auto tmp_dir = fs::temp_directory_path() / "workx_subprocess_interactive_cwd_test";
    fs::create_directories(tmp_dir);
    const std::string marker = "unique_marker_file_xyz.txt";
    auto marker_path = tmp_dir / marker;
    {
        std::ofstream ofs(marker_path);
        ofs << "marker";
    }

    // 子进程在指定 cwd 下探测文件存在性（静默，exit 0=存在 / 1=不存在）
#ifdef _WIN32
    auto r = exec_interactive("cmd.exe", {"/c", "if exist " + marker + " (exit 0) else (exit 1)"},
                              tmp_dir.string());
#else
    auto r = exec_interactive("/bin/sh", {"-c", "test -f '" + marker + "'"}, tmp_dir.string());
#endif

    REQUIRE(r.is_ok());
    REQUIRE(r.value().exit_code == 0);

    std::error_code ec;
    fs::remove(marker_path, ec);
    fs::remove(tmp_dir, ec);
}
