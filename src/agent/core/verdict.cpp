#include "agent/core/verdict.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/process/exec_output.h"
#include "core/process/subprocess.h"
#include "liblogger/logger.h"

namespace fs = std::filesystem;

namespace agent {

namespace {

/// @brief 去掉首尾空白的原始串
std::string trimmed(std::string_view s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(s.begin(), s.end(), not_space);
    auto last = std::find_if(s.rbegin(), s.rend(), not_space).base();
    if (first >= last) {
        return {};  // 全空白/空串：避免用 first>last 构造（UB）
    }
    return std::string(first, last);
}

/// @brief 去首尾空白并转小写（仅供目标类型前缀匹配，不用于取值）
/// @details P2-3：类型前缀判定需大小写不敏感，但 file_exists:<path> / cmd:<cmd>
///          的"值"必须保留原样（Linux 文件路径大小写敏感），故本函数只用于
///          判断 v == type 或 starts_with(prefix)，取值一律走 trimmed() 原文。
std::string lowered_trim(std::string_view s) {
    std::string out = trimmed(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

// ============================================================
// P1-1：cmd: 命令白名单 — 阻止任意 shell 命令执行
// ============================================================
// 威胁模型：agent.goal / goal.command 来自应用级配置（用户自担风险），
// 但若项目级 .mcp.json 或恶意 skill 能污染配置值，裸 `sh -c <config>` /
// `cmd.exe /d /s /c <config>` 即构成 RCE。深度防御：仅放行已知构建/测试
// 工具，并在包装 shell（cmd/sh）后递归校验真正落地的命令 token。

/// @brief 拆出命令串中"将真实执行"的 token（去掉 shell 包装层）
/// @details 处理：
///           - 前导空白 / 引号包裹的"名称"（"cmake" / 'g++'）
///           - 包装 shell：cmd /C|/S /d|/D ...、sh -c ... → 跳过其 flag，
///             返回后一个真正命令 token。
std::string command_exec_token(std::string_view line) {
    // 按空白切 token（命令串第一段通常无 shell 元字符；dock 到 /;&&| 之前）
    std::vector<std::string> tokens;
    std::string cur;
    for (char c : line) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
        } else if (c == ';' || c == '&' || c == '|' || c == '>' || c == '<' || c == '`') {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            break;  // 出现 shell 元字符：只信任前面的命令段，其余先按整体拒
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) tokens.push_back(cur);

    const auto strip_quote = [](std::string t) {
        if (t.size() >= 2 && (t.front() == '"' || t.front() == '\'') && t.back() == t.front()) {
            t = t.substr(1, t.size() - 2);
        }
        return t;
    };

    const std::string first = tokens.empty() ? "" : strip_quote(tokens[0]);
    if (first.empty()) return {};

    // 包装 shell：跳过其启动 flag，返回真实命令 token
    if (first == "cmd" || first == "cmd.exe" || first == "sh" || first == "bash" ||
        first == "powershell" || first == "pwsh" || first == "zsh" || first == "dash") {
        for (size_t i = 1; i < tokens.size(); ++i) {
            const std::string t = strip_quote(tokens[i]);
            if (t.empty()) continue;
            if (t.front() == '-' || t.front() == '/') {
                continue;  // -c / -Command / /C / /d / /s ... 均跳过
            }
            return t;
        }
        return first;  // 只有包装层本身（如 `sh`）→ 保守放行纯包装
    }
    return first;
}

/// @brief 命令串是否含 shell 链接/分隔/重定向/命令替换元字符（P1-1 硬阻塞修复）
/// @details guard_command 必须在字符串级拒绝这些字符，而非仅校验首个 token：
///          即使首命令在白名单内（如 cmake），`&&` 之后的任意命令仍会被包装
///          shell（sh -c / cmd /c）执行 → RCE。默认命令（kTestCmd/kBuildCmd/
///          kLintCmd）是项目硬编码可信串，不含这些字符，放行不受影响。
bool contains_separator(std::string_view s) noexcept {
    for (const char c : s) {
        switch (c) {
            case ';':   // 命令分隔
            case '&':   // 后台 / &&
            case '|':   // 管道 / ||
            case '>':   // 重定向
            case '<':   // 重定向
            case '`':   // 反引号命令替换
            case '$':   // 变量展开 / $(...) 命令替换
            case '(':   // 子 shell / 命令分组
            case ')':   // 子 shell
            case '\n':  // 换行命令分隔
            case '\r':
                return true;
            default:
                break;
        }
    }
    return false;
}

/// @brief 命令是否在白名单内（仅校验真实落地的 exec token）
bool is_command_allowed(std::string_view line) noexcept {
    // P1-1：构建/测试/包管理/常见脚本工具白名单。默认命令（kTestCmd/kBuildCmd
    //       等）是项目硬编码的可信串，不在此校验范围内；此处只拦截 goal.command
    //       里的任意命令。
    static constexpr std::string_view kAllowed[] = {
        // 构建
        "cmake",
        "ctest",
        "ninja",
        "make",
        "nmake",
        "msbuild",
        "dotnet",
        "cargo",
        "go",
        "gcc",
        "g++",
        "clang",
        "clang++",
        "cl",
        // 包管理
        "npm",
        "npx",
        "yarn",
        "pnpm",
        "pip",
        "pip3",
        "conan",
        "vcpkg",
        "mvn",
        "gradle",
        // 脚本/语言运行
        "python",
        "python3",
        "py",
        "pytest",
        "node",
        // 版本控制
        "git",
        // 基础 shell 元命令（褪去包装层后的落点，如 `cmd /C exit 0`）
        "echo",
        "true",
        "false",
        "exit",
        "test",
    };
    const std::string tok = command_exec_token(line);
    if (tok.empty()) return false;
    for (auto a : kAllowed) {
        if (tok == a) return true;
    }
    return false;
}

/// @brief P2-2：detail 字符串清洗——不原样回显命令/路径，防二次注入到 TUI/LLM
std::string sanitize_detail(std::string_view raw) noexcept {
    // 只截断长度 + 剔除控制字符；命令/路径内容不嵌入 detail，改由退出码/状态表达
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t') {
            continue;  // 丢弃非可打印控制字符
        }
        out.push_back(c);
    }
    if (out.size() > 120) {
        out.resize(120);
        out += "...";
    }
    return out;
}

}  // namespace

/// @brief 执行命令并返回退出码；启动失败返回 -1
int run_exit_code(const std::string& cmd, const std::string& cwd) {
    using namespace agent::process;
    // exec() 的 cmd 必须是纯可执行名、参数走 args；命令字符串需经 shell 包装
    // （cmd：cmd.exe /d /s /c；POSIX：sh -c），对齐 skill hooks.cpp 的既有用法
    // #78 VF-08：验证是自动执行的，stdin 必须接空设备 —— 否则继承父进程
    //            stdin，命令一旦读输入就会挂到 60s 超时才返回。
#if defined(_WIN32)
    auto res = exec("cmd.exe", ExecOptions{
                                   .cwd = cwd,
                                   .args = {"/d", "/s", "/c", cmd},
                                   .timeout = std::chrono::milliseconds(60000),
                                   .stdin_mode = StdinMode::Null,
                               });
#else
    auto res = exec("sh", ExecOptions{
                              .cwd = cwd,
                              .args = {"-c", cmd},
                              .timeout = std::chrono::milliseconds(60000),
                              .stdin_mode = StdinMode::Null,
                          });
#endif
    if (res.is_err()) {
        LOG_WARN("[verdict] exec failed cmd='{}': {}", cmd, res.error().message);
        return -1;
    }
    return res.value().exit_code;
}

/// @brief 默认测试命令（可被 goal.command 覆盖）
const char* kTestCmd = "ctest --output-on-failure";
/// 默认编译（Windows 用 cmake --build；非 Windows 同，CMake 跨平台）
const char* kBuildCmd = "cmake --build . --config Debug";

/// @brief 校验一条待执行命令（默认命令白名单直通；覆盖命令需在白名单内）
/// @return 允许则返回原命令；被拦截则返回空
///
/// P1-1：goal.command 来自配置，若被污染即可任意执行 shell 命令（RCE）。
///        默认命令（kTestCmd 等）是项目硬编码的可信串→放行；
///        非空覆盖命令→必须在白名单内，否则拒绝执行（返回 Failed）。
std::string guard_command(const std::string& cmd) {
    // P1-1 硬阻塞：命令串含 shell 链接/分隔/重定向/命令替换元字符时，整个拒绝。
    // 不能只靠 command_exec_token（它遇 `&&` 即停并返回首 token"cmake"，导致
    // "cmake --build . && rm -rf /" 被当成白名单命令放行 → 包装 shell 全量执行 → RCE。
    if (contains_separator(cmd)) {
        LOG_WARN("[guard_command] rejected: contains shell separator/operator");
        return {};
    }
    if (is_command_allowed(cmd)) {
        return cmd;
    }
    return {};
}

// ============================================================
// #78 P4：验证命令自适应探测
// ============================================================

namespace {

/// @brief 读取文件前若干字节；读不到返回空串
/// @details 仅用于探测项目线索，不做严格错误处理。
std::string read_text_head(const fs::path& p, size_t max_bytes = 256 * 1024) {
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return {};
    }
    std::string out(max_bytes, '\0');
    in.read(out.data(), static_cast<std::streamsize>(max_bytes));
    out.resize(static_cast<size_t>(in.gcount()));
    return out;
}

}  // namespace

std::string detect_goal_command(AgentGoal::Type type, const std::string& cwd) {
    std::error_code ec;
    const fs::path base = cwd.empty() ? fs::current_path() : fs::path(cwd);
    if (!fs::is_directory(base, ec)) {
        return {};
    }
    const auto has = [&](std::string_view rel) { return fs::exists(base / rel, ec); };
    const auto text_of = [&](std::string_view rel) { return read_text_head(base / rel); };

    switch (type) {
        case AgentGoal::TestsPass:
            // CMake：必须真的生成过 CTest 入口才算「有测试」——
            // 只有 CMakeLists.txt 而没有 CTestTestfile，说明尚未 configure，
            // 跑 ctest 必然报错，属于「无法验证」，不该当成「测试失败」。
            if (has("CMakeLists.txt") &&
                (has("CTestTestfile.cmake") || has("build/CTestTestfile.cmake"))) {
                return kTestCmd;
            }
            if (has("Cargo.toml")) {
                return "cargo test";
            }
            if (has("go.mod")) {
                return "go test ./...";
            }
            if (has("package.json") &&
                text_of("package.json").find("\"test\"") != std::string::npos) {
                return "npm test";
            }
            if (has("Makefile") && text_of("Makefile").find("\ntest:") != std::string::npos) {
                return "make test";
            }
            if (has("pytest.ini") || has("setup.py") || has("pyproject.toml") || has("tests")) {
                return "pytest";
            }
            return {};

        case AgentGoal::BuildClean:
            if (has("CMakeLists.txt")) {
                return kBuildCmd;
            }
            if (has("Cargo.toml")) {
                return "cargo build";
            }
            if (has("go.mod")) {
                return "go build ./...";
            }
            if (has("Makefile")) {
                return "make";
            }
            if (has("package.json") &&
                text_of("package.json").find("\"build\"") != std::string::npos) {
                return "npm run build";
            }
            return {};

        case AgentGoal::LintZero:
            // 只覆盖能给出完整可跑命令的 lint 栈。.clang-tidy 单独跑无参数
            // 会 usage error，给出它反而制造假失败，故不探测。
            if (has(".eslintrc") || has(".eslintrc.json") || has(".eslintrc.js") ||
                has("eslint.config.js")) {
                return "npx eslint .";
            }
            return {};

        default:
            // None / FileExists / CustomScript：命令由 goal 自身给定，无探测必要
            return {};
    }
}

AgentGoal detect_default_goal(const std::string& cwd) {
    AgentGoal goal;
    if (!detect_goal_command(AgentGoal::TestsPass, cwd).empty()) {
        goal.type = AgentGoal::TestsPass;
        return goal;
    }
    if (!detect_goal_command(AgentGoal::BuildClean, cwd).empty()) {
        goal.type = AgentGoal::BuildClean;
        return goal;
    }
    return goal;  // None：项目里没有可验证的东西，门禁放行
}

Verdict checker_tests(const AgentGoal& goal, const std::string& cwd) {
    // #78 P4：未指定命令时先探测；探测不到则报「不可用」由门禁放行。
    const std::string cmd =
        goal.command.empty() ? detect_goal_command(AgentGoal::TestsPass, cwd) : goal.command;
    if (cmd.empty()) {
        return {GoalStatus::Unknown, "no test command detected in this project", true};
    }
    const std::string guarded = guard_command(cmd);
    if (guarded.empty()) {
        return {GoalStatus::Failed, "test command rejected (not in allowlist)"};
    }
    const int code = run_exit_code(guarded, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, "test command failed to start"};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "all tests pass"};
    }
    return {GoalStatus::Pending, std::format("tests failing (exit={})", code)};
}

Verdict checker_build(const AgentGoal& goal, const std::string& cwd) {
    // 同 checker_tests：未显式指定命令时先探测。
    const std::string raw =
        goal.command.empty() ? detect_goal_command(AgentGoal::BuildClean, cwd) : goal.command;
    if (raw.empty()) {
        return {GoalStatus::Unknown, "no build command detected in this project", true};
    }
    const std::string cmd = guard_command(raw);
    if (cmd.empty()) {
        return {GoalStatus::Failed, "build command rejected (not in allowlist)"};
    }
    const int code = run_exit_code(cmd, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, "build command failed to start"};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "build clean (zero errors)"};
    }
    return {GoalStatus::Pending, std::format("build has errors (exit={})", code)};
}

Verdict checker_lint(const AgentGoal& goal, const std::string& cwd) {
    // #78 P4：lint 此前的默认命令是 `echo 'no lint config'` —— 它恒定退出 0，
    // 等于「没有 lint 配置也永远判定通过」，是假绿。改为探测不到就
    // 如实报不可用（门禁放行），不再用 echo 假装验证过。
    const std::string raw =
        goal.command.empty() ? detect_goal_command(AgentGoal::LintZero, cwd) : goal.command;
    if (raw.empty()) {
        return {GoalStatus::Unknown, "no lint command detected in this project", true};
    }
    const std::string cmd = guard_command(raw);
    if (cmd.empty()) {
        return {GoalStatus::Failed, "lint command rejected (not in allowlist)"};
    }
    const int code = run_exit_code(cmd, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, "lint command failed to start"};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "lint zero warnings"};
    }
    return {GoalStatus::Pending, std::format("lint has warnings (exit={})", code)};
}

Verdict checker_file_exists(const AgentGoal& goal, const std::string& cwd) {
    if (goal.path.empty()) {
        return {GoalStatus::Failed, "file_exists goal requires path"};
    }
    // P2-1：路径规范化 + 隔离检查——把相对/绝对路径都解析为基于 cwd 的绝对形式，
    //        再用"父子包含"判定是否逃逸出 cwd（防 file_exists:../../.ssh/id_rsa 探测
    //        任意文件）。P2-3：路径大小写保持原名，不做 lowered。
    std::error_code ec;
    const fs::path base = fs::weakly_canonical(fs::path(cwd), ec);  // cwd 规范化
    if (ec) {
        return {GoalStatus::Failed, "cannot resolve working directory"};
    }
    // 绝对路径：按字面规范化（仍须落在 cwd 内才会通过父关系判定）
    fs::path target = fs::path(goal.path);
    if (target.is_relative()) {
        target = base / target;
    }
    const fs::path canon = fs::weakly_canonical(target, ec);
    if (ec) {
        return {GoalStatus::Failed,
                sanitize_detail(std::format("cannot resolve path: {}", goal.path))};
    }
    // 判定 canon 是否在 base 子树内（自身或位于 base 之下）
    const std::string base_s = base.lexically_normal().string();
    const std::string canon_s = canon.lexically_normal().string();
    bool inside = false;
    if (!base_s.empty() && !canon_s.empty()) {
        if (canon_s == base_s) {
            inside = true;
        } else {
            const std::string prefix = base_s + std::string(1, fs::path::preferred_separator);
            inside = canon_s.rfind(prefix, 0) == 0;
        }
    }
    if (!inside) {
        return {GoalStatus::Failed, "file_exists: path escapes working directory"};
    }
    const bool ok = fs::exists(canon, ec);
    if (ec) {
        return {GoalStatus::Failed, std::format("cannot stat target: {}", ec.message())};
    }
    if (ok) {
        return {GoalStatus::Achieved, "file exists"};
    }
    return {GoalStatus::Pending, "file missing"};
}

Verdict checker_custom(const AgentGoal& goal, const std::string& cwd) {
    if (goal.command.empty()) {
        return {GoalStatus::Failed, "custom_script goal requires command"};
    }
    // P1-1：自定义命令同样受白名单约束
    const std::string cmd = guard_command(goal.command);
    if (cmd.empty()) {
        return {GoalStatus::Failed, "custom script rejected (not in allowlist)"};
    }
    const int code = run_exit_code(cmd, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, "custom script failed to start"};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "custom script exited 0"};
    }
    return {GoalStatus::Pending, std::format("custom script exited {}", code)};
}

Verdict check_goal(const AgentGoal& goal, const std::string& cwd) {
    switch (goal.type) {
        case AgentGoal::TestsPass:
            return checker_tests(goal, cwd);
        case AgentGoal::BuildClean:
            return checker_build(goal, cwd);
        case AgentGoal::LintZero:
            return checker_lint(goal, cwd);
        case AgentGoal::FileExists:
            return checker_file_exists(goal, cwd);
        case AgentGoal::CustomScript:
            return checker_custom(goal, cwd);
        case AgentGoal::Script:
        case AgentGoal::Batch:
        case AgentGoal::Watch:
            // #32：多模式目标是专属 Loop 驱动的多步执行（并行/轮询），不是单一
            //       命令可判定的 Verdict。若被错误路由到 GoalGuarded，直接 Failed，
            //       避免 check_goal 被误判为 Pending 造成死循环。
            return {GoalStatus::Failed,
                    "goal type is driven by its dedicated agent (script/batch/watch)"};
        case AgentGoal::None:
        default:
            return {GoalStatus::Pending, "no goal"};
    }
}

bool has_checker(AgentGoal::Type type) noexcept {
    switch (type) {
        case AgentGoal::TestsPass:
        case AgentGoal::BuildClean:
        case AgentGoal::LintZero:
        case AgentGoal::FileExists:
        case AgentGoal::CustomScript:
            return true;
        case AgentGoal::None:
        default:
            return false;
    }
}

namespace {

/// @brief 前缀匹配（按子串前缀，如 "file_exists:" 与 "cmd:" 需要冒号分隔）
bool starts_with(std::string_view v, std::string_view prefix) noexcept {
    return v.size() >= prefix.size() && v.substr(0, prefix.size()) == prefix;
}

}  // namespace

AgentGoal parse_goal(std::string_view spec) noexcept {
    // P2-3：类型前缀判定大小写不敏感（lowered_trim），但 file_exists:/cmd: 的
    //       值用 trimmed 原文提取，保留文件路径 / 命令的原始大小写（Linux 需要）。
    const std::string v_low = lowered_trim(spec);
    AgentGoal goal;
    if (v_low.empty()) {
        return goal;  // None
    }
    if (v_low == "tests_pass" || v_low == "tests" || v_low == "test" || v_low == "ctest") {
        goal.type = AgentGoal::TestsPass;
        return goal;
    }
    if (v_low == "build_clean" || v_low == "build" || v_low == "compile") {
        goal.type = AgentGoal::BuildClean;
        return goal;
    }
    if (v_low == "lint_zero" || v_low == "lint") {
        goal.type = AgentGoal::LintZero;
        return goal;
    }
    if (starts_with(v_low, "file_exists:")) {
        goal.type = AgentGoal::FileExists;
        // 取值用原文（仅去掉原始首尾空白），保留大小写
        goal.path = trimmed(spec).substr(std::string_view("file_exists:").size());
        return goal;
    }
    if (starts_with(v_low, "cmd:")) {
        goal.type = AgentGoal::CustomScript;
        goal.command = trimmed(spec).substr(std::string_view("cmd:").size());
        return goal;
    }
    // ---- #32 多模式目标：script: / batch: / watch: ----
    if (starts_with(v_low, "script:")) {
        goal.type = AgentGoal::Script;
        goal.command = trimmed(spec).substr(std::string_view("script:").size());
        return goal;
    }
    if (starts_with(v_low, "batch:") || starts_with(v_low, "watch:")) {
        const bool is_watch = starts_with(v_low, "watch:");
        const std::string_view sfx =
            is_watch ? std::string_view("watch:") : std::string_view("batch:");
        const std::string rest = trimmed(spec).substr(sfx.size());
        // & 分隔的 k=v 键值表
        std::vector<std::pair<std::string, std::string>> kv;
        std::stringstream ss(rest);
        std::string tok;
        while (std::getline(ss, tok, '&')) {
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) {
                kv.emplace_back(tok, "");  // 裸键（如 concurrency）值为空
            } else {
                kv.emplace_back(tok.substr(0, eq), tok.substr(eq + 1));
            }
        }
        goal.type = is_watch ? AgentGoal::Watch : AgentGoal::Batch;
        for (auto& [k, val] : kv) {
            if (k == "cmd")
                goal.command = val;
            else if (k == "glob")
                goal.glob = val;
            else if (k == "path")
                goal.path = val;
            else if (k == "concurrency") {
                try {
                    goal.concurrency = std::max(1, std::stoi(val));
                } catch (...) {
                }
            } else if (k == "polls") {
                try {
                    goal.watch_polls = std::max(1, std::stoi(val));
                } catch (...) {
                }
            } else if (k == "interval") {
                try {
                    goal.watch_interval_ms = std::max(0, std::stoi(val));
                } catch (...) {
                }
            }
        }
        return goal;
    }
    // 无法识别 → None（不抛异常，避免配置错误崩会话）
    return goal;
}

}  // namespace agent