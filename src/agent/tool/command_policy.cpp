/**
 * @file command_policy.cpp
 * @brief #85：命令拦截白名单严格档实现
 * @details 词法判定，不依赖 shell 解析器。所有分支都走「从严」：凡是静态无法确定
 *          载荷的写法（包装器 / 变量 / 命令替换 / 重定向）一律降级为 Ask。
 * @version 1.0.0
 * @date 2026-10
 */

#include "agent/tool/command_policy.h"

#include <algorithm>
#include <cctype>
#include <format>

#include "agent/audit/audit_logger.h"
#include "agent/tool/permission_ask.h"
#include "agent/tool/shell_guard.h"

namespace agent::tool {

namespace {

/// @brief 命令分隔符判定（引号外调用）
/// @param[out] width 分隔符宽度（&& 为 2）
/// @details `2>&1` 中的 `&` **不是**分隔符：若在此切分，
///          `cmake --build . 2>&1` 会被误切成两条并误伤为"需确认"。
bool is_subcommand_separator(std::string_view s, size_t i, size_t& width) {
    width = 1;
    const char c = s[i];
    if (c == ';' || c == '\n' || c == '|') return true;
    if (c != '&') return false;
    if (i + 1 < s.size() && s[i + 1] == '&') {
        width = 2;
        return true;
    }
    if (i > 0 && s[i - 1] == '>') return false;  // 2>&1：fd 复制，非分隔
    return true;
}

/// @brief 按命令分隔符切分复合命令（引号内不切分）
std::vector<std::string> split_subcommands(std::string_view command) {
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    for (size_t i = 0; i < command.size();) {
        const char c = command[i];
        if (quote != 0) {
            cur.push_back(c);
            if (c == quote) quote = 0;
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur.push_back(c);
            ++i;
            continue;
        }
        size_t w = 1;
        if (is_subcommand_separator(command, i, w)) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            i += w;
            continue;
        }
        cur.push_back(c);
        ++i;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

/// @brief 空白分词（引号感知；不切分 `>`/`<`，保留引号供 unquote 剥除）
std::vector<std::string> tokenize(std::string_view text) {
    std::vector<std::string> tokens;
    std::string cur;
    char quote = 0;
    for (char c : text) {
        if (quote != 0) {
            cur.push_back(c);
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur.push_back(c);
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!cur.empty()) {
                tokens.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) tokens.push_back(cur);
    return tokens;
}

/// @brief 剥掉 token 首尾引号
std::string unquote(std::string_view token) {
    std::string t(token);
    while (t.size() >= 2 &&
           ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\''))) {
        t = t.substr(1, t.size() - 2);
    }
    return t;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// @brief 可执行名归一化：去目录、去扩展名、转小写
/// @details `C:\Tools\Git.EXE` / `/usr/bin/git` / `git.cmd` 一律归一为 `git`。
std::string exe_basename(std::string_view token) {
    std::string s = to_lower(token);
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    for (std::string_view ext : {".exe", ".cmd", ".bat", ".com", ".ps1"}) {
        if (s.size() > ext.size() && s.compare(s.size() - ext.size(), ext.size(), ext) == 0) {
            s = s.substr(0, s.size() - ext.size());
            break;
        }
    }
    return s;
}

/// @brief 静态无法确定载荷的包装器（一律降级为 Ask）
/// @details 这些程序的参数内容才决定实际执行什么，白名单无法覆盖其语义。
bool is_wrapper(std::string_view exe) {
    for (std::string_view w : {"sh",   "bash", "zsh",  "dash",   "ksh",     "fish",  "powershell",
                               "pwsh", "cmd",  "eval", "xargs",  "sudo",    "su",    "nohup",
                               "env",  "wsl",  "ssh",  "docker", "kubectl", "start", "script"}) {
        if (exe == w) return true;
    }
    return false;
}

/// @brief 解释器集合：接收管道输入即等于执行任意上游载荷
/// @details `curl … | sh` / `base64 -d | bash` 是远程代码执行的典型形态，
///          载荷不出现在命令行里，白名单与黑名单都无从判定 → 严格档直接拒绝。
bool is_interpreter(std::string_view exe) {
    for (std::string_view w : {"sh", "bash", "zsh", "dash", "ksh", "fish", "powershell", "pwsh",
                               "python", "python3", "perl", "ruby", "node", "eval"}) {
        if (exe == w) return true;
    }
    return false;
}

/// @brief 破坏性 find：`-delete` / `-exec` 系列
/// @details shell_guard 的破坏性表只覆盖 rm / mkfs / dd / format，`find / -delete`
///          与 `find . -exec rm {} \;` 是等价删除路径却漏网（issue #85 点名的绕过形态）。
///          严格档下这些是不可恢复操作，直接拒绝而不询问。
bool is_destructive_find(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return false;
    if (exe_basename(unquote(tokens.front())) != "find") return false;
    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string t = unquote(tokens[i]);
        for (std::string_view flag : {"-delete", "-exec", "-execdir", "-ok", "-okdir"}) {
            if (t == flag) return true;
        }
    }
    return false;
}

/// @brief token 是否为重定向（`>` `>>` `<` `>>file` `2>err` `&>file`）
/// @details `2>&1` / `1>&2` 是 fd 复制而非重定向，必须放过 —— 否则
///          `cmake --build . 2>&1` 这类最常规的构建命令会被误伤。
bool is_redirect_token(const std::string& raw) {
    const size_t p = raw.find_first_of("<>");
    if (p == std::string::npos) return false;
    return p + 1 >= raw.size() || raw[p + 1] != '&';
}

/// @brief 子命令是否含重定向或命令替换
/// @details 二者都能把白名单命令的效果导向白名单之外（`ctest > /etc/x`），
///          故命中即降级 Ask，而不是按可执行名放行。
bool has_redirect_or_substitution(const std::vector<std::string>& tokens) {
    for (const auto& raw : tokens) {
        if (is_redirect_token(raw)) return true;
        if (raw.find('`') != std::string::npos) return true;
        if (raw.find("$(") != std::string::npos) return true;
    }
    return false;
}

/// @brief 白名单表：token 前缀
/// @details 选取原则：① issue 点名的 `cmake --build` / `ctest` / `git status` /
///          `python -m pytest`；② 构建与测试工具链的受控动作；③ 纯过滤器。
///          刻意不放行：`git push` / `npm install` / `pip install` / `curl` /
///          `python script.py` —— 均有副作用或不可控输入。
const std::vector<std::vector<std::string>> kCommandWhitelist = {
    // 版本控制（只读）
    {"git", "status"},
    {"git", "diff"},
    {"git", "log"},
    {"git", "show"},
    {"git", "branch"},
    {"git", "rev-parse"},
    {"git", "describe"},
    {"git", "ls-files"},
    // 构建 / 测试（issue 点名）
    {"cmake", "--build"},
    {"cmake", "--version"},
    {"ctest"},
    {"python", "-m", "pytest"},
    {"python3", "-m", "pytest"},
    {"python", "-m", "unittest"},
    {"python3", "-m", "unittest"},
    {"pytest"},
    {"dotnet", "build"},
    {"dotnet", "test"},
    {"cargo", "build"},
    {"cargo", "test"},
    {"go", "build"},
    {"go", "test"},
    {"go", "vet"},
    {"npm", "test"},
    {"make"},
    {"ninja"},
    // 纯过滤器（stdin → stdout，不落盘）
    {"head"},
    {"tail"},
    {"grep"},
    {"rg"},
    {"sort"},
    {"wc"},
    // PowerShell 只读 cmdlet（跨平台表之外的补充；大小写已归一）
    {"get-childitem"},
    {"get-content"},
    {"select-string"},
};

/// @brief 单条子命令判定
StrictDecision classify_subcommand(const std::vector<std::string>& tokens) {
    if (tokens.empty()) return {StrictVerdict::Deny, "empty sub-command"};
    const std::string head = unquote(tokens.front());
    if (head.empty()) return {StrictVerdict::Deny, "empty sub-command"};
    if (has_redirect_or_substitution(tokens)) {
        return {StrictVerdict::Ask, "redirection or command substitution is not auto-approved"};
    }
    if (head.front() == '$' || head.find("${") != std::string::npos) {
        return {StrictVerdict::Ask, "dynamic command name is not auto-approved"};
    }
    const std::string exe = exe_basename(head);
    if (exe.empty()) return {StrictVerdict::Deny, "unparsable command name"};
    if (is_wrapper(exe)) {
        return {StrictVerdict::Ask, "shell/elevation wrapper '" + exe + "' needs approval"};
    }
    // 归一化 token 序列：首 token 用 basename，其余原样小写
    std::vector<std::string> norm;
    norm.reserve(tokens.size());
    norm.push_back(exe);
    for (size_t i = 1; i < tokens.size(); ++i) norm.push_back(to_lower(unquote(tokens[i])));
    for (const auto& entry : kCommandWhitelist) {
        if (norm.size() >= entry.size() && std::equal(entry.begin(), entry.end(), norm.begin())) {
            return {StrictVerdict::Allow, ""};
        }
    }
    return {StrictVerdict::Ask, "'" + exe + "' is not in the strict-mode whitelist"};
}

}  // namespace

std::string_view strict_verdict_name(StrictVerdict v) noexcept {
    switch (v) {
        case StrictVerdict::Allow:
            return "allow";
        case StrictVerdict::Ask:
            return "ask";
        case StrictVerdict::Deny:
            return "deny";
    }
    return "ask";
}

const std::vector<std::vector<std::string>>& strict_command_whitelist() noexcept {
    return kCommandWhitelist;
}

StrictDecision evaluate_strict_command(std::string_view command) {
    // 1) 风险门禁：命中即拒绝 —— 严格档下不该问用户"要不要 rm -rf /"
    const ShellRisk risk = detect_shell_risk(command);
    if (risk != ShellRisk::None) {
        return {StrictVerdict::Deny, "blocked by security guard: " + shell_risk_description(risk)};
    }
    // 2) 逐条子命令判定；任一条需要确认 → 整体需要确认
    const auto subs = split_subcommands(command);
    if (subs.empty()) return {StrictVerdict::Deny, "no executable command found"};
    std::vector<std::vector<std::string>> toks;
    toks.reserve(subs.size());
    for (const auto& sub : subs) toks.push_back(tokenize(sub));
    // 2a) 拒绝优先：先整条扫一遍，再判白名单。否则 `curl x | bash` 里 curl 的 Ask
    //     会抢先返回，把真正危险的管道到解释器降级成"可确认"——这正是验收标准要堵的洞。
    //     仅命中"非首条"（前面有 `|`/`;`），首条的 `sh -c …` 仍走 2b 的 Ask。
    for (size_t i = 0; i < toks.size(); ++i) {
        if (i > 0 && !toks[i].empty()) {
            const std::string exe = exe_basename(unquote(toks[i].front()));
            if (is_interpreter(exe)) {
                return {StrictVerdict::Deny,
                        "piping into interpreter '" + exe + "' is blocked in strict mode"};
            }
        }
        if (is_destructive_find(toks[i])) {
            return {StrictVerdict::Deny,
                    "destructive find (-delete/-exec) is blocked in strict mode"};
        }
    }
    // 2b) 常规白名单判定：任一条需要确认 → 整体需要确认
    for (const auto& t : toks) {
        const auto d = classify_subcommand(t);
        if (d.verdict != StrictVerdict::Allow) return d;
    }
    return {StrictVerdict::Allow, ""};
}

namespace {

/// @brief 把判定结果落审计（放行也记，便于事后核对"为什么这条被放行"）
void audit_decision(const StrictDecision& d, std::string_view command, const ToolContext& ctx,
                    std::string_view tool_name) {
    audit::AuditEvent ev;
    ev.type = audit::EventType::ToolPermissionDecision;
    ev.severity =
        (d.verdict == StrictVerdict::Allow) ? audit::Severity::Info : audit::Severity::Warn;
    ev.session_id = ctx.session_id;
    ev.request_id = ctx.request_id;
    ev.tool_name = std::string(tool_name);
    ev.input = nlohmann::json{{"command", std::string(command)}};
    ev.decision = std::string(strict_verdict_name(d.verdict));
    ev.decision_reason = d.reason.empty() ? std::string("strict whitelist match") : d.reason;
    ev.security_flags = {"strict_mode"};
    audit::AuditLogger::instance().log(std::move(ev));
}

/// @brief Ask 档的确认问句
std::string ask_question(const StrictDecision& d, std::string_view command) {
    return std::format(
        "Strict mode: this command is not auto-approved ({}):\n\n```\n{}\n```\n\n"
        "Allow running it once?",
        d.reason.empty() ? std::string("not whitelisted") : d.reason, command);
}

}  // namespace

StrictEnforceResult enforce_strict_command(std::string_view command, const ToolContext& ctx,
                                           std::string_view tool_name) {
    const auto d = evaluate_strict_command(command);
    audit_decision(d, command, ctx, tool_name);
    if (d.verdict == StrictVerdict::Allow) return {true, ""};
    if (d.verdict == StrictVerdict::Deny) return {false, d.reason};
    if (!ask_user_confirm(ctx, ask_question(d, command))) {
        return {false, "Command denied in strict mode: " + d.reason};
    }
    return {true, ""};
}

}  // namespace agent::tool
