/**
 * @file shell_guard.cpp
 * @brief Shell 命令安全守卫实现
 * @details 命令级词法检查（无 AST 解析，覆盖 #35 期望的最小基线）：
 *          - 破坏性命令黑名单（rm -rf /、mkfs、dd if=/dev/、format、shutdown 等）
 *          - SSRF 防护（curl/wget/iwr 指向内网/云元数据地址）
 *          - 环境变量泄露（env / printenv / /proc/<pid>/environ / PowerShell env:）
 *          - cwd 限制（在项目根或 allowlist 内）
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/tool/shell_guard.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <regex>
#include <string>

namespace agent::tool {

namespace {

/// @brief ASCII 小写
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// @brief URL host 是否命中内网/元数据网段
/// @details 支持 IPv4 与 localhost；IPv4-mapped IPv6（::ffff:a.b.c.d）一并拦截。
///          仅纯数字 host 参与网段判断（域名不做 DNS 解析，纯函数约束）。
bool is_private_ip_host(std::string_view host) {
    auto to_u32 = [](unsigned a, unsigned b, unsigned c, unsigned d) {
        return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
               (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
    };
    auto in_range = [&](uint32_t ip, uint32_t base, uint32_t mask) {
        return (ip & mask) == (base & mask);
    };

    // ::ffff:1.2.3.4 剥掉 IPv4-mapped 前缀
    std::string_view h = host;
    constexpr std::string_view kMapped = "::ffff:";
    if (h.rfind(kMapped, 0) == 0) h.remove_prefix(kMapped.size());

    if (lower(h) == "localhost") return true;

    // 解析 IPv4
    unsigned a[4] = {0, 0, 0, 0};
    int parts = 0;
    std::string cur;
    for (size_t i = 0; i <= h.size(); ++i) {
        const char c = (i < h.size()) ? h[i] : '\0';
        if (c == '.') {
            if (parts >= 4 || cur.empty()) return false;
            if (cur.size() > 3) return false;
            for (char ch : cur) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
            }
            a[parts++] = static_cast<unsigned>(std::stoul(cur));
            cur.clear();
        } else if (c == '\0') {
            break;
        } else {
            cur.push_back(c);
        }
    }
    const std::string last(cur);
    if (!last.empty()) {
        if (parts >= 4) return false;
        for (char ch : last) {
            if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
        }
        a[parts++] = static_cast<unsigned>(std::stoul(last));
    }
    if (parts != 4) return false;  // 非 IPv4（域名/主机名）不在本层判断

    const uint32_t ip = to_u32(a[0], a[1], a[2], a[3]);
    // 云元数据 / 链路本地：169.254.0.0/16（含 169.254.169.254）
    if (in_range(ip, 0xA9FE0000, 0xFFFF0000)) return true;
    // CGNAT：100.64.0.0/10
    if (in_range(ip, 0x64400000, 0xFFC00000)) return true;
    // 回环：127.0.0.0/8
    if (in_range(ip, 0x7F000000, 0xFF000000)) return true;
    // 私网：10.0.0.0/8
    if (in_range(ip, 0x0A000000, 0xFF000000)) return true;
    // 私网：172.16.0.0/12
    if (in_range(ip, 0xAC100000, 0xFFF00000)) return true;
    // 私网：192.168.0.0/16
    if (in_range(ip, 0xC0A80000, 0xFFFF0000)) return true;
    // 0.0.0.0/8
    if (in_range(ip, 0x00000000, 0xFF000000)) return true;
    return false;
}

/// @brief 命令中的裸 URL 是否指向内网 host
/// @details 提取命令中 http(s):// 后的 host（到 / ' " 空白 结束），
///          再做 is_private_ip_host 判断。裸 IP（如 169.254.169.254）也拦截。
bool url_ssrf(std::string_view cmd) {
    const std::string c = lower(cmd);
    size_t pos = 0;
    while ((pos = c.find("http", pos)) != std::string::npos) {
        // http:// 或 https://
        size_t scheme_end = pos + 4;
        if (scheme_end < c.size() && c[scheme_end] == 's') ++scheme_end;
        if (scheme_end >= c.size() || c.substr(scheme_end, 3) != "://") {
            pos = scheme_end;
            continue;
        }
        const size_t host_start = scheme_end + 3;
        size_t host_end = host_start;
        while (host_end < c.size()) {
            const char ch = c[host_end];
            if (ch == '/' || ch == '"' || ch == '\'' || ch == ' ' || ch == '\t' || ch == '&' ||
                ch == ';' || ch == '|' || ch == '>') {
                break;
            }
            ++host_end;
        }
        const std::string_view host(c.data() + host_start, host_end - host_start);
        if (host.empty()) {
            pos = host_end;
            continue;
        }
        // 剥掉 userinfo@
        const size_t at = host.rfind('@');
        std::string_view clean = (at == std::string_view::npos) ? host : host.substr(at + 1);
        // 剥显式端口（H-2R：须在括号处理之前，且仅当最后一段为纯数字，
        // 避免误剥 [::ffff:169.254.169.254]:8080 的 IPv6 内部冒号）
        const size_t colon = clean.rfind(':');
        if (colon != std::string_view::npos &&
            std::all_of(clean.begin() + colon + 1, clean.end(),
                        [](unsigned char ch) { return std::isdigit(ch); })) {
            clean = clean.substr(0, colon);
        }
        // 剥 IPv6 括号（is_private_ip_host 内部处理 ::ffff: 前缀）
        if (!clean.empty() && clean.front() == '[' && clean.back() == ']') {
            clean = clean.substr(1, clean.size() - 2);
        }
        if (is_private_ip_host(clean)) return true;
        pos = host_end;
    }
    // 裸元数据地址（无 scheme 的 curl http 由上面覆盖；此处兜底裸 IP 直连工具）
    return false;
}

/// @brief 命令 token 化（引号感知；保留原始大小写）
/// @details 空白与管道符（| ; & && || > <）处切分，双/单引号内不切分。
std::vector<std::string> tokenize(std::string_view command) {
    std::vector<std::string> tokens;
    std::string cur;
    char quote = 0;
    for (char c : command) {
        if (quote != 0) {
            if (c == quote) quote = 0;
            cur.push_back(c);  // 引号保留在 token 内，判定时剥除
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            cur.push_back(c);
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) || c == '|' || c == '&' || c == ';' ||
            c == '>' || c == '<') {
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

/// @brief 是否命中破坏性命令模式
bool destructive_regex(std::string_view command) {
    // 统一小写匹配（Windows 命令大小写不敏感；POSIX 大写路径罕见，宽进严出）
    const std::string c = lower(command);

    // rm 危险目标：token 化后判断（rm -rf /、/*、~、$HOME、*、.、..）
    {
        const auto tokens = tokenize(c);
        for (size_t i = 0; i + 1 < tokens.size(); ++i) {
            const std::string& tok = tokens[i];
            // 命令名须为 rm（或路径 rm）
            const std::string cmd = unquote(tok);
            const size_t slash = cmd.find_last_of("/\\");
            const std::string base = slash == std::string::npos ? cmd : cmd.substr(slash + 1);
            if (base != "rm") continue;
            // 找开关 token，须含 r/f
            bool has_rf = false;
            size_t j = i + 1;
            for (; j < tokens.size(); ++j) {
                const std::string t = unquote(tokens[j]);
                if (t.empty() || t[0] != '-') break;
                if (t.find('r') != std::string::npos || t.find('f') != std::string::npos)
                    has_rf = true;
            }
            if (!has_rf || j >= tokens.size()) continue;
            const std::string target = unquote(tokens[j]);
            // 递归式 rm -r dir、-f 文件不拦截；仅根/家/宏/全量模式
            if (target == "/" || target == "/*" || target == "~" || target == "$home" ||
                target == "*" || target == "." || target == "..") {
                return true;
            }
        }
    }

    // mkfs / mkfs.ext4 等
    if (std::regex_search(c, std::regex(R"(\bmkfs\b)"))) return true;
    // dd if=/dev/…（磁盘级读写）
    if (std::regex_search(c, std::regex(R"(\bdd\s+.*\bif=/dev/)"))) return true;
    // of=/dev/sd…（裸设备写入）
    if (std::regex_search(c, std::regex(R"(\bof=/dev/sd)"))) return true;
    // > /dev/sd（重定向到裸设备）
    if (std::regex_search(c, std::regex(R"(>\s*/dev/sd)"))) return true;
    // Windows format C: / format.com
    if (std::regex_search(c, std::regex(R"(\bformat\s+[a-z]:)"))) return true;
    if (std::regex_search(c, std::regex(R"(\bformat\.com\b)"))) return true;
    // Windows 级联删除 del /f/s/q + 盘符根（允许任意数量开关）
    if (std::regex_search(c, std::regex(R"(\bdel\s+(?:/[a-z]+\s+)+[a-z]:\\?)"))) return true;
    if (std::regex_search(c, std::regex(R"(\b(?:rmdir|rd)\s+/(?:s|q)+\s+[a-z]:\\?)"))) return true;
    // 关机/重启
    if (std::regex_search(c, std::regex(R"(\b(shutdown|reboot|halt|poweroff)\b)"))) return true;
    if (std::regex_search(c, std::regex(R"(\binit\s+0\b)"))) return true;
    // 注册表 HKLM 级联删除
    if (std::regex_search(c, std::regex(R"(\breg\s+delete\s+hklm)"))) return true;
    return false;
}

/// @brief 是否泄露环境变量
/// @details token 化判定：独立 token env/printenv 后若无 "KEY=VAL" 设置形式即为泄露。
bool env_leak_regex(std::string_view command) {
    const std::string c = lower(command);
    const auto tokens = tokenize(c);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string tok = unquote(tokens[i]);
        if (tok == "env" || tok == "printenv") {
            // 排除设置形式：env KEY=VAL cmd（下一个 token 含 '=' 且不含路径分隔符）
            if (i + 1 < tokens.size()) {
                const std::string next = unquote(tokens[i + 1]);
                if (next.find('=') != std::string::npos && next.find('/') == std::string::npos &&
                    next.find('\\') == std::string::npos) {
                    continue;  // 设置环境变量的合法用法
                }
            }
            return true;
        }
    }
    // /proc/*/environ（cat /proc/self/environ）
    if (std::regex_search(c, std::regex(R"(/proc/[^/\s]+/environ)"))) return true;
    // PowerShell Get-ChildItem env: / gci env: / ls env:
    if (std::regex_search(c, std::regex(R"(gci\s+env:)"))) return true;
    if (std::regex_search(c, std::regex(R"((?:get-childitem|ls)\s+env:)"))) return true;
    return false;
}

// ============================================================
// #84 方案 A：沙盒目标级检测（restrictive 档的执行前兜底）
// ============================================================

/// 敏感系统路径前缀（小写）。Windows 前缀在 '\\' 归一形式上匹配，
/// POSIX 前缀在原始小写形式上匹配。
constexpr std::string_view kSensitiveWinPaths[] = {
    "c:\\windows", "c:\\program files", "c:\\programdata",
};
constexpr std::string_view kSensitivePosixPaths[] = {
    "/etc", "/usr", "/bin", "/sbin", "/boot", "/lib",
};

/// 写动作 token（需与敏感路径近邻共现才判定为写意图，窗口 64 字符）。
/// 检索时带词边界：字母开头 token 的前一字符不得为字母/数字
/// （避免 "cmd " 命中 "md "、"model " 命中 "del "、"scp " 命中 "cp "）。
constexpr std::string_view kWriteActionTokens[] = {
    ">", ">>", "del ", "rd ", "rmdir", "rm ", "mv ", "move ", "copy ", "xcopy ", "robocopy ",
    "echo ", "touch ", "mkdir ", "cp ", "tee ", "dd ", "format ", "mkfs", "icacls ", "takeown ",
    "attrib ", "sed -i", "new-item ", "set-content", "add-content ", "out-file", "remove-item",
    "install ", "unzip ", "tar -x", "expand ", "msiexec", "curl -o ", "curl --output",
    "wget -o ", "wget --output",
};

/// 写意图判定窗口：写动作 token 结束位置到敏感路径起始位置的最大距离
constexpr size_t kWriteProximityWindow = 64;

/// prefix 在 form 中的所有「路径级」出现位置。
/// 下一个字符须为路径分隔/引号/空白/数字等边界（避免 "/etc" 命中 "/etcetera"）；
/// 前一个字符须非字母（避免 "x/usr" 误命中）。
std::vector<size_t> find_path_prefix(const std::string& form, const std::string& prefix) {
    std::vector<size_t> hits;
    const auto is_boundary = [](char ch) {
        return ch == '/' || ch == '\\' || ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' ||
               ch == ';' || ch == '|' || ch == '&' || ch == '>' || ch == '<' || ch == ')' ||
               ch == ',' || std::isdigit(static_cast<unsigned char>(ch));
    };
    size_t pos = 0;
    while ((pos = form.find(prefix, pos)) != std::string::npos) {
        const bool prev_ok = pos == 0 || !std::isalpha(static_cast<unsigned char>(form[pos - 1]));
        const size_t end = pos + prefix.size();
        const bool next_ok = end >= form.size() || is_boundary(form[end]);
        if (prev_ok && next_ok) hits.push_back(pos);
        pos += prefix.size();
    }
    return hits;
}

/// 敏感路径出现位置前方窗口内是否存在写动作 token（字母开头 token 带词边界）
bool has_write_action_near(const std::string& c, size_t path_pos) {
    const size_t begin = path_pos > kWriteProximityWindow ? path_pos - kWriteProximityWindow : 0;
    for (const auto& tok : kWriteActionTokens) {
        const std::string_view t(tok);
        const bool need_word_boundary =
            std::isalpha(static_cast<unsigned char>(t.front())) != 0;
        size_t p = c.find(t, begin);
        while (p != std::string::npos && p < path_pos) {
            if (p + t.size() <= path_pos) {
                const bool boundary_ok =
                    !need_word_boundary || p == 0 ||
                    !std::isalnum(static_cast<unsigned char>(c[p - 1]));
                if (boundary_ok) return true;
            }
            p = c.find(t, p + 1);
        }
    }
    return false;
}

/// 敏感系统路径写意图（写动作 token 与敏感路径近邻共现）。
/// c:\windows\temp 豁免——restrictive 档本就允许写临时目录（SandboxConfig::allow_temp_write）。
bool sensitive_system_write(const std::string& c, const std::string& win_form,
                            const SandboxTargetPolicy& policy) {
    auto hit_with_write = [&](const std::string& form, const std::string& prefix, bool win_style) {
        for (const size_t pos : find_path_prefix(form, prefix)) {
            if (win_style && form.compare(pos, 15, "c:\\windows\\temp") == 0) {
                const size_t after = pos + 15;
                if (after >= form.size() || form[after] == '\\' || form[after] == '"' ||
                    form[after] == ' ') {
                    continue;  // 纯临时目录写入，豁免
                }
            }
            if (has_write_action_near(form, pos)) return true;
        }
        return false;
    };

    for (const auto& p : kSensitiveWinPaths) {
        if (hit_with_write(win_form, std::string(p), true)) return true;
    }
    for (const auto& p : kSensitivePosixPaths) {
        if (hit_with_write(c, std::string(p), false)) return true;
    }
    // 用户配置的 deny_write 前缀：分隔符双向归一（条目的 '/'→'\\' 与 '\\'→'/'），
    // 分别与原始小写命令（'/' 形式）和归一形式（'\\' 形式）交叉匹配
    for (const auto& raw : policy.deny_write) {
        const std::string as_is = lower(raw);
        std::string bs = as_is;
        std::replace(bs.begin(), bs.end(), '/', '\\');
        std::string fs = as_is;
        std::replace(fs.begin(), fs.end(), '\\', '/');
        if (hit_with_write(c, as_is, false) || hit_with_write(c, fs, false) ||
            hit_with_write(win_form, as_is, false) || hit_with_write(win_form, bs, false)) {
            return true;
        }
    }
    return false;
}

/// HKLM 注册表写意图（HKLM 引用 + 写类命令/cmdlet 共现；
/// `reg delete hklm` 已由破坏性档覆盖，此处补充其余写形态）
bool registry_hklm_write(const std::string& c) {
    if (c.find("hklm") == std::string::npos &&
        c.find("hkey_local_machine") == std::string::npos) {
        return false;
    }
    constexpr std::string_view kRegWriteTokens[] = {
        "reg add", "reg delete", "reg import", "reg load", "reg restore", "reg unload",
        "regedit", "set-itemproperty", "new-itemproperty", "remove-itemproperty",
    };
    for (const auto& t : kRegWriteTokens) {
        if (c.find(t) != std::string::npos) return true;
    }
    return false;
}

/// URL host 是否命中域名白名单（支持 sandbox_config.h 的 `*.example.com` 通配语义）
bool host_allowed(std::string_view host, const std::vector<std::string>& allow_domains) {
    for (const auto& raw : allow_domains) {
        const std::string dom = lower(raw);
        if (dom.rfind("*.", 0) == 0) {
            // "*.example.com" → host 等于 example.com 或以 ".example.com" 结尾
            const std::string suffix = dom.substr(1);
            if (host == dom.substr(2) ||
                (host.size() >= suffix.size() &&
                 host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                return true;
            }
            continue;
        }
        if (host == dom) return true;
    }
    return false;
}

/// 网络访问意图：
/// 1. 任一 http(s) URL 的 host 在白名单外（白名单为空 = 全部拦截）→ 拦截；
/// 2. 白名单为空时的网络工具启发式（curl/wget/git push/pip install 等）。
///    白名单非空时跳过纯工具名启发式——无 URL 无法判断目标域，从严会误伤白名单语义。
bool network_use(const std::string& c, const SandboxTargetPolicy& policy) {
    if (!policy.network_isolated) return false;

    bool has_url = false;
    size_t pos = 0;
    while ((pos = c.find("http", pos)) != std::string::npos) {
        size_t scheme_end = pos + 4;
        if (scheme_end < c.size() && c[scheme_end] == 's') ++scheme_end;
        if (scheme_end >= c.size() || c.compare(scheme_end, 3, "://") != 0) {
            pos = scheme_end;
            continue;
        }
        const size_t host_start = scheme_end + 3;
        size_t host_end = host_start;
        while (host_end < c.size()) {
            const char ch = c[host_end];
            if (ch == '/' || ch == '"' || ch == '\'' || ch == ' ' || ch == '\t' || ch == '&' ||
                ch == ';' || ch == '|' || ch == '>' || ch == ':') {
                break;
            }
            ++host_end;
        }
        if (host_end > host_start) {
            has_url = true;
            const std::string_view host(c.data() + host_start, host_end - host_start);
            if (!host_allowed(host, policy.allow_domains)) return true;
        }
        pos = host_end;
    }
    if (has_url) return false;  // 所有 URL 均在白名单内（未在循环里拦截）

    if (!policy.allow_domains.empty()) return false;  // 白名单模式：不再启发式误伤

    static const std::regex kNetTools(
        R"(\b(curl|wget|ftp|telnet|nc|ncat|netcat|bitsadmin|ssh|scp|sftp)\b)"
        R"(|\binvoke-webrequest\b|\binvoke-restmethod\b|\biwr\b|\birm\b)"
        R"(|\bgit\s+(?:push|pull|fetch|clone|ls-remote)\b)"
        R"(|\bpip3?\s+(?:install|download)\b)"
        R"(|\bnpm\s+(?:install|publish|update|add)\b)"
        R"(|\byarn\s+add\b|\bpnpm\s+(?:add|install)\b)"
        R"(|\bwinget\s+install\b|\bchoco\s+install\b|\bscoop\s+install\b)"
        R"(|\bcertutil\b[^|;&]*-urlcache)");
    return std::regex_search(c, kNetTools);
}

}  // namespace

bool contains_destructive_command(std::string_view command) { return destructive_regex(command); }

bool is_ssrf_target(std::string_view command) { return url_ssrf(command); }

bool leaks_env_vars(std::string_view command) { return env_leak_regex(command); }

ShellRisk detect_shell_risk(std::string_view command) {
    uint32_t risk = static_cast<uint32_t>(ShellRisk::None);
    if (contains_destructive_command(command))
        risk |= static_cast<uint32_t>(ShellRisk::Destructive);
    if (is_ssrf_target(command)) risk |= static_cast<uint32_t>(ShellRisk::SSRF);
    if (leaks_env_vars(command)) risk |= static_cast<uint32_t>(ShellRisk::EnvLeak);
    return static_cast<ShellRisk>(risk);
}

std::string shell_risk_description(ShellRisk risk) {
    std::string desc;
    auto append = [&desc](std::string_view part) {
        if (!desc.empty()) desc += ", ";
        desc += part;
    };
    if ((risk & ShellRisk::Destructive) != ShellRisk::None) append("destructive command");
    if ((risk & ShellRisk::SSRF) != ShellRisk::None) append("request to internal/metadata address");
    if ((risk & ShellRisk::EnvLeak) != ShellRisk::None) append("may leak environment variables");
    return desc;
}

bool is_command_cwd_allowed(std::string_view cwd, std::string_view base,
                            const std::vector<std::string>& allowlist) {
    if (cwd.empty() || base.empty()) return false;
    std::error_code ec;
    const std::filesystem::path p(cwd);
    if (!p.is_absolute()) return false;
    const std::filesystem::path norm = std::filesystem::weakly_canonical(p, ec);
    if (ec) return false;

    auto under = [](std::string_view p, std::string_view root) {
        if (root.empty()) return false;
        if (p == root) return true;
        if (p.size() > root.size() && p.compare(0, root.size(), root) == 0) {
            const char next = p[root.size()];
            return next == '/' || next == '\\';
        }
        return false;
    };
    const std::string n = norm.generic_string();
    // 评审 H-3：base/allowlist 也统一为 / 分隔，避免 Windows 反斜杠误拒
    const std::string b = std::filesystem::path(base).generic_string();
    if (under(n, b)) return true;
    for (const auto& root : allowlist) {
        if (under(n, std::filesystem::path(root).generic_string())) return true;
    }
    return false;
}

SandboxTargetRisk detect_sandbox_target_risk(std::string_view command,
                                             const SandboxTargetPolicy& policy) {
    uint32_t risk = static_cast<uint32_t>(SandboxTargetRisk::None);
    const std::string c = lower(command);
    // Windows 路径分隔符归一：用户写 c:/windows 与 c:\windows 等价
    std::string win_form = c;
    std::replace(win_form.begin(), win_form.end(), '/', '\\');

    if (sensitive_system_write(c, win_form, policy))
        risk |= static_cast<uint32_t>(SandboxTargetRisk::SystemWrite);
    if (registry_hklm_write(c))
        risk |= static_cast<uint32_t>(SandboxTargetRisk::RegistryWrite);
    if (network_use(c, policy)) risk |= static_cast<uint32_t>(SandboxTargetRisk::NetworkUse);
    return static_cast<SandboxTargetRisk>(risk);
}

std::string sandbox_target_risk_description(SandboxTargetRisk risk) {
    std::string desc;
    auto append = [&desc](std::string_view part) {
        if (!desc.empty()) desc += ", ";
        desc += part;
    };
    if ((risk & SandboxTargetRisk::SystemWrite) != SandboxTargetRisk::None)
        append("write to protected system locations");
    if ((risk & SandboxTargetRisk::RegistryWrite) != SandboxTargetRisk::None)
        append("HKLM registry modification");
    if ((risk & SandboxTargetRisk::NetworkUse) != SandboxTargetRisk::None)
        append("network access while sandboxed");
    return desc;
}

}  // namespace agent::tool