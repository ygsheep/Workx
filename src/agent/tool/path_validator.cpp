/**
 * @file path_validator.cpp
 * @brief 统一路径安全校验实现
 * @details 覆盖 #34 期望的安全机制：
 *          - CWD 边界强制（canonical 解析 symlink 后双重检查）
 *          - 敏感文件/目录硬编码拦截
 *          - Windows 可疑路径模式（UNC / 8.3 / ADS / 长路径前缀 / 尾点空格）
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/path_validator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

#include "core/utils/error.h"

namespace agent::tool {

namespace fs = std::filesystem;

namespace {

/// @brief 路径段小写化（Windows/大小写不敏感文件系统的归一化防御）
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// @brief 敏感文件名（命中即拦截）
const std::unordered_set<std::string>& sensitive_files() {
    static const std::unordered_set<std::string> k = {
        ".env",        ".gitconfig",  ".git-credentials", ".netrc",
        ".npmrc",      ".pypirc",     ".bashrc",          ".bash_profile",
        ".zshrc",      ".zprofile",   ".profile",         "id_rsa",
        "id_dsa",      "id_ecdsa",    "id_ed25519",       "authorized_keys",
        "known_hosts", "credentials", "passwd",           "shadow",
        "sam",         "ntuser.dat",
    };
    return k;
}

/// @brief 敏感目录段（路径任一目录段命中即拦截）
/// @note 评审 #3：不再整目录拦截 `config`/`hooks`（误伤正常项目 ht 目录）；
///       `.git/hooks` 语境由 path_contains_sensitive 单独限定；`~/.config` 下
///       的敏感子项（.ssh/.aws 等）已单列。
const std::unordered_set<std::string>& sensitive_dirs() {
    static const std::unordered_set<std::string> k = {
        ".ssh", ".gnupg", ".aws", ".azure", ".gcloud", ".kube",
    };
    return k;
}

/// @brief 文件是否命中敏感清单（含 .env.xxx 前缀变体）
bool is_sensitive_filename(std::string_view filename) {
    const std::string name = lower(filename);
    if (sensitive_files().count(name) > 0) return true;
    // .env.local / .env.production 等变体
    if (name.rfind(".env.", 0) == 0) return true;
    return false;
}

/// @brief 提取最后一个文件名段（末尾含 / 时取前一段）
std::string filename_segment(std::string_view path) {
    std::string_view p = path;
    // 去掉尾部分隔符
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) {
        p.remove_suffix(1);
    }
    const size_t pos = p.find_last_of("/\\");
    return std::string(pos == std::string_view::npos ? p : p.substr(pos + 1));
}

/// @brief 检查路径中任一目录段命中敏感目录（最后一个段按文件名检查）
/// @note 评审 #3：`hooks` 仅在 `.git/hooks` 语境下拦截，避免误伤普通 hooks 目录。
bool path_contains_sensitive(std::string_view path) {
    std::string cur;
    std::string prev;  // 上一个目录段（用于 .git/hooks 语境判定）
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) {
                if (sensitive_dirs().count(lower(cur)) > 0) return true;
                if (lower(prev) == ".git" && lower(cur) == "hooks") return true;
                prev = cur;
                cur.clear();
            }
        } else {
            cur.push_back(c);
        }
    }
    // 最后一个段按文件名规则检查（同时检查目录命中）
    if (!cur.empty()) {
        if (sensitive_dirs().count(lower(cur)) > 0) return true;
        if (is_sensitive_filename(cur)) return true;
        if (lower(prev) == ".git" && lower(cur) == "hooks") return true;
    }
    // Windows 盘符 C: 单独出现时不是文件段
    return false;
}

/// @brief 8.3 短名检测（GIT~1 / BASHR~1 模式：8 字符内 + ~ + 数字）
bool is_short_name(std::string_view seg) {
    // 段必须含 ~，~ 后为数字，~ 前为非 ~ 字符
    const size_t tilde = seg.find('~');
    if (tilde == std::string_view::npos) return false;
    if (tilde == 0) return false;
    // ~ 后全是数字
    for (size_t i = tilde + 1; i < seg.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(seg[i]))) return false;
    }
    return true;
}

}  // namespace

bool has_suspicious_windows_pattern(std::string_view path) {
    if (path.empty()) return false;

    // 1. 本地设备长路径前缀（\\?\C:\...）— 归一化防御，拒绝
    if (path.rfind("\\\\?\\", 0) == 0 || path.rfind("//?/", 0) == 0) return true;

    // 2. UNC 路径（\\host\share 或 //host/share）— 可能触发 NTLM 凭据泄露
    if (path.rfind("\\\\", 0) == 0 || path.rfind("//", 0) == 0) return true;

    // 3. 逐段检查：ADS / 8.3 短名 / 尾点空格
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        const char c = (i < path.size()) ? path[i] : '\0';
        if (c == '/' || c == '\\' || c == '\0') {
            if (!cur.empty()) {
                // 3a. NTFS 备用数据流（file::$DATA / file:stream）
                if (cur.find("::") != std::string::npos) return true;
                // 3b. 8.3 短名
                if (is_short_name(cur)) return true;
                // 3c. 尾点/尾空格（Windows 解析时剥离，可绕过扩展名/敏感拦截）
                const char last = cur.back();
                if (last == '.' || last == ' ') return true;
            }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return false;
}

bool matches_sensitive_path(std::string_view path) { return path_contains_sensitive(path); }

bool is_absolutely_forbidden_path(std::string_view path) {
    // 评审 #2：绝对禁止（私钥/凭据/系统账户文件）——不可用户确认放行
    static const std::unordered_set<std::string> kForbiddenFiles = {
        "id_rsa",      "id_dsa",      "id_ecdsa",         "id_ed25519", "authorized_keys",
        "known_hosts", "credentials", ".git-credentials", ".netrc",     "passwd",
        "shadow",      "sam",         "ntuser.dat",
    };
    // 存私钥/凭据的目录：绝对禁止
    static const std::unordered_set<std::string> kForbiddenDirs = {
        ".ssh", ".gnupg", ".aws", ".azure", ".gcloud", ".kube",
    };
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty() && kForbiddenDirs.count(lower(cur)) > 0) return true;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) {
        if (kForbiddenDirs.count(lower(cur)) > 0) return true;
        if (kForbiddenFiles.count(lower(cur)) > 0) return true;
    }
    return false;
}

bool is_within_allowed_root(std::string_view path, std::string_view cwd,
                            const std::vector<std::string>& allowlist) {
    // 评审 #4：Windows 文件系统大小写不敏感，前缀比较前统一小写，避免
    // 合法路径（cwd 与解析结果大小写不一致）被误判为越界。
    // 同时统一分隔符：canonical.generic_string() 产出正斜杠，而 cwd/expand_path
    // 在 Windows 上可能为反斜杠，字符串前缀比较前必须归一化，否则 cwd 内路径
    // 也会因 '/' 与 '\' 不匹配被误判越界。
    const auto norm = [](std::string_view s) -> std::string {
        std::string out(s);
#ifdef _WIN32
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::replace(out.begin(), out.end(), '\\', '/');
#endif
        return out;
    };
    const std::string np = norm(path);
    auto under = [&np, &norm](std::string_view root) {
        const std::string nr = norm(root);
        if (nr.empty()) return false;
        if (np == nr) return true;
        if (np.size() > nr.size() && np.compare(0, nr.size(), nr) == 0) {
            const char next = np[nr.size()];
            return next == '/' || next == '\\';
        }
        return false;
    };
    if (under(cwd)) return true;
    for (const auto& root : allowlist) {
        if (under(root)) return true;
    }
    return false;
}

ResultV2<void> validate_path_access(std::string_view path, std::string_view cwd,
                                    const std::vector<std::string>& allowlist) {
    if (path.empty()) {
        return ResultV2<void>::err(Error::Code::PermissionDenied, "Path is empty");
    }
    if (has_suspicious_windows_pattern(path)) {
        return ResultV2<void>::err(
            Error::Code::PermissionDenied,
            "Path contains a suspicious Windows pattern (UNC / short name / ADS / "
            "long-path prefix / trailing dot-space) and is blocked");
    }
    if (matches_sensitive_path(path)) {
        return ResultV2<void>::err(
            Error::Code::PermissionDenied,
            "Path matches a sensitive file/directory (e.g. .env, .ssh, credentials) "
            "and is blocked");
    }

    // 边界检查：存在时 canonical（解析 symlink），不存在时 weakly_canonical。
    // 两个结果都校验：symlink 指向边界外的场景由 canonical 结果拦截。
    const fs::path p(path);
    std::error_code ec;
    const fs::path canonical = fs::canonical(p, ec);
    if (!ec) {
        if (!is_within_allowed_root(canonical.generic_string(), cwd, allowlist)) {
            return ResultV2<void>::err(Error::Code::PermissionDenied,
                                       "Path is outside the allowed working directory");
        }
        return ResultV2<void>::ok();
    }
    ec.clear();
    const fs::path weakly = fs::weakly_canonical(p, ec);
    if (!ec) {
        if (!is_within_allowed_root(weakly.generic_string(), cwd, allowlist)) {
            return ResultV2<void>::err(Error::Code::PermissionDenied,
                                       "Path escapes the allowed working directory");
        }
        return ResultV2<void>::ok();
    }
    // 评审 #1（fail-closed）：canonical 与 weakly_canonical 均解析失败时，
    // 保守拒绝而非放行，避免畸形/权限受限路径绕过边界与敏感拦截。
    // 仅当 ec 明确为"路径不存在"这类无害情形时放行（写入新文件等场景）。
    if (ec == std::errc::no_such_file_or_directory) {
        return ResultV2<void>::ok();
    }
    return ResultV2<void>::err(Error::Code::PermissionDenied,
                               "Unable to resolve path for boundary/environment check");
}

}  // namespace agent::tool