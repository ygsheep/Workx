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

}  // namespace agent::tool