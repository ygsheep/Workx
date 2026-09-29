/**
 * @file audit_logger.cpp
 * @brief 审计日志系统实现（#37）
 * @details JSONL 格式写入、脱敏、轮转、过期清理。
 *          脱敏复用 agent::tool::scan_for_secrets 规则集。
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/audit/audit_logger.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <filesystem>

#include "agent/tool/secret_scanner.h"
#include "agent/tool/encoding.h"

namespace agent::audit {

namespace fs = std::filesystem;

// ============================================================================
// 枚举转字符串
// ============================================================================

std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::ToolInvoke:
            return "tool.invoke";
        case EventType::ToolPermissionDecision:
            return "tool.permission.decision";
        case EventType::ToolPermissionDenied:
            return "tool.permission.denied";
        case EventType::ToolResultTruncated:
            return "tool.result.truncated";
        case EventType::SecurityDangerousCommand:
            return "security.dangerous_command";
        case EventType::SecuritySensitiveFile:
            return "security.sensitive_file";
        case EventType::SecuritySecretDetected:
            return "security.secret_detected";
        case EventType::SecuritySandboxDisabled:
            return "security.sandbox_disabled";
        case EventType::SecuritySandboxDegraded:
            return "security.sandbox_degraded";
        case EventType::SecurityPathTraversal:
            return "security.path_traversal";
        case EventType::SecuritySymlinkBypass:
            return "security.symlink_bypass";
        case EventType::SecuritySSRFAttempt:
            return "security.ssrf_attempt";
        case EventType::SecurityEnvVarLeak:
            return "security.env_var_leak";
        case EventType::SessionStart:
            return "session.start";
        case EventType::SessionEnd:
            return "session.end";
        case EventType::AgentStart:
            return "agent.start";
        case EventType::AgentDone:
            return "agent.done";
        case EventType::AgentInterrupted:
            return "agent.interrupted";
        case EventType::AgentMaxIterations:
            return "agent.max_iterations";
    }
    return "unknown";
}

std::string_view to_string(Severity sev) noexcept {
    switch (sev) {
        case Severity::Info:
            return "info";
        case Severity::Warn:
            return "warn";
        case Severity::Critical:
            return "critical";
    }
    return "unknown";
}

// ============================================================================
// AuditLogger 单例
// ============================================================================

AuditLogger& AuditLogger::instance() noexcept {
    static AuditLogger inst;
    return inst;
}

AuditLogger::~AuditLogger() { flush(); }

void AuditLogger::init(const std::string& file_path, size_t max_size_mb, size_t retention_days) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_path_ = file_path;
    max_size_bytes_ = max_size_mb * 1024 * 1024;
    retention_days_ = retention_days;

    // 创建目录
    fs::path p(file_path_);
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }

    initialized_ = true;
    enabled_ = true;

    // 清理过期日志
    cleanup_expired();
}

// ============================================================================
// 核心日志方法
// ============================================================================

void AuditLogger::log(AuditEvent event) {
    if (!enabled_ || !initialized_) return;

    std::string line = serialize(event);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_ || !initialized_) return;

    {
        // 以 append 模式写入（append-only，不可篡改历史）
        std::ofstream ofs(file_path_, std::ios::app);
        if (ofs.is_open()) {
            ofs << line << '\n';
            ofs.flush();
        }
    }  // ofs 先关闭，Windows 下 rename 才能成功（共享冲突）

    // 检查轮转
    maybe_rotate();
}

// ============================================================================
// 便捷方法
// ============================================================================

void AuditLogger::log_tool_invoke(const std::string& tool_name, const nlohmann::json& input,
                                  const std::string& session_id, const std::string& request_id,
                                  std::string_view decision, std::string_view decision_reason,
                                  int64_t duration_ms, std::string_view output,
                                  const std::vector<std::string>& security_flags) {
    AuditEvent ev;
    ev.type = EventType::ToolInvoke;
    ev.severity = security_flags.empty() ? Severity::Info : Severity::Warn;
    ev.session_id = session_id;
    ev.request_id = request_id;
    ev.tool_name = tool_name;
    ev.input = redact_input(input);
    ev.decision = std::string(decision);
    ev.decision_reason = std::string(decision_reason);
    ev.duration_ms = duration_ms;
    ev.output_summary = redact(std::string(output)).substr(0, 200);
    ev.security_flags = security_flags;
    ev.user = current_user_host();

    log(std::move(ev));
}

void AuditLogger::log_security(EventType type, const std::string& detail,
                               const std::string& session_id, std::string_view tool_name) {
    AuditEvent ev;
    ev.type = type;
    ev.severity = Severity::Critical;
    ev.session_id = session_id;
    ev.tool_name = std::string(tool_name);
    ev.input = {{"detail", redact(detail)}};
    ev.user = current_user_host();

    log(std::move(ev));
}

void AuditLogger::log_session_lifecycle(EventType type, const std::string& session_id) {
    AuditEvent ev;
    ev.type = type;
    ev.severity = Severity::Info;
    ev.session_id = session_id;
    ev.user = current_user_host();
    log(std::move(ev));
}

void AuditLogger::log_agent_lifecycle(EventType type, const std::string& session_id,
                                      int iteration) {
    AuditEvent ev;
    ev.type = type;
    ev.severity = (type == EventType::AgentInterrupted || type == EventType::AgentMaxIterations)
                      ? Severity::Warn
                      : Severity::Info;
    ev.session_id = session_id;
    ev.user = current_user_host();
    if (iteration > 0) {
        ev.input = {{"iteration", iteration}};
    }
    log(std::move(ev));
}

void AuditLogger::flush() {
    // ofstream 在每次 log() 时已 flush，此处为接口完整性保留
}

// ============================================================================
// 序列化
// ============================================================================

std::string AuditLogger::serialize(const AuditEvent& event) const {
    nlohmann::json j;
    j["timestamp"] = timestamp_now();
    j["event_type"] = std::string(to_string(event.type));
    j["severity"] = std::string(to_string(event.severity));

    if (!event.session_id.empty()) j["session_id"] = event.session_id;
    if (!event.request_id.empty()) j["request_id"] = event.request_id;
    if (!event.trace_id.empty()) j["trace_id"] = event.trace_id;
    if (!event.tool_name.empty()) j["tool_name"] = event.tool_name;
    if (!event.input.is_null()) j["input"] = event.input;
    if (!event.decision.empty()) j["decision"] = event.decision;
    if (!event.decision_reason.empty()) j["decision_reason"] = event.decision_reason;
    if (event.duration_ms > 0) j["duration_ms"] = event.duration_ms;
    if (!event.output_summary.empty()) j["output_summary"] = event.output_summary;
    if (!event.security_flags.empty()) j["security_flags"] = event.security_flags;
    if (!event.user.empty()) j["user"] = event.user;

    // UTF-8 兜底：递归清洗所有字符串字段，避免工具入参/输出含非 UTF-8
    // 字节（如 GBK 子进程 stdout）时 j.dump() 抛出 nlohmann 的 type_error.316。
    return tool::sanitize_json_strings(j).dump();
}

// ============================================================================
// 脱敏
// ============================================================================

std::string AuditLogger::redact(const std::string& text) const {
    // 复用 SecretScanner 规则集逐处替换为 [REDACTED:label]
    // 不得保留明文前缀（#37 场景 5：密钥绝不进入审计日志）
    // 先清洗非法 UTF-8 字节，避免下游 serialize 的 j.dump() 抛 type_error.316
    return tool::redact_secrets(tool::sanitize_utf8(text));
}

nlohmann::json AuditLogger::redact_input(const nlohmann::json& input) const {
    if (!input.is_object()) return input;

    // 先清洗非 UTF-8 字节，避免下方 input.dump().size() 抛 type_error.316
    const nlohmann::json clean = tool::sanitize_json_strings(input);

    // 提取关键字段（避免记录全量输入导致日志爆炸）
    // 优先提取：file_path / command / pattern / path / url / cwd
    static const std::vector<std::string> kKeyFields = {"file_path", "command", "pattern", "path",
                                                        "url",       "cwd",     "query"};

    nlohmann::json redacted;
    for (const auto& key : kKeyFields) {
        if (clean.contains(key)) {
            const auto& val = clean[key];
            if (val.is_string()) {
                redacted[key] = redact(val.get<std::string>());
            } else {
                redacted[key] = val;
            }
        }
    }

    // 记录输入大小（非敏感元信息）
    redacted["_size"] = clean.dump().size();

    return redacted;
}

// ============================================================================
// 轮转与清理
// ============================================================================

void AuditLogger::maybe_rotate() {
    // 必须在 mutex_ 持锁状态下调用
    std::error_code ec;
    auto size = fs::file_size(file_path_, ec);
    if (ec || size <= static_cast<uintmax_t>(max_size_bytes_)) return;

    // 关闭当前文件（ofstream 已在 log() 中关闭）
    // 重命名为带时间戳的轮转文件
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);

    fs::path rotated = fs::path(file_path_).concat("." + std::string(ts));
    fs::rename(file_path_, rotated, ec);
    // 轮转失败不阻断后续日志（下次 log 会重建文件）
}

void AuditLogger::cleanup_expired() {
    // 必须在 mutex_ 持锁状态下调用
    if (file_path_.empty()) return;

    fs::path dir = fs::path(file_path_).parent_path();
    if (dir.empty()) return;

    auto now = std::chrono::system_clock::now();
    auto cutoff = now - std::chrono::hours(24 * retention_days_);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;

        // 仅清理 audit.*.jsonl 格式的轮转文件
        auto name = entry.path().filename().string();
        if (name.find("audit.") != 0 || name.find(".jsonl") == std::string::npos) continue;
        // 跳过当前活动文件
        if (entry.path().string() == file_path_) continue;

        auto mtime = fs::last_write_time(entry.path(), ec);
        if (ec) continue;
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            mtime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        if (sctp < cutoff) {
            fs::remove(entry.path(), ec);
        }
    }
}

// ============================================================================
// 时间戳
// ============================================================================

std::string AuditLogger::timestamp_now() const {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif

    // 计算时区偏移（分钟）
    auto offset_min = [](const std::tm& t) -> int {
#ifdef _WIN32
        (void)t;
        long tz_offset;
        _get_timezone(&tz_offset);
        return static_cast<int>(-tz_offset / 60);
#else
        return t.tm_gmtoff / 60;
#endif
    }(tm);

    char sign = offset_min >= 0 ? '+' : '-';
    int oh = std::abs(offset_min) / 60;
    int om = std::abs(offset_min) % 60;

    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(3)
       << ms.count() << sign << std::setfill('0') << std::setw(2) << oh << ':' << std::setfill('0')
       << std::setw(2) << om;
    return ss.str();
}

std::string AuditLogger::current_user_host() const {
    const char* user = std::getenv("USERNAME");
    if (!user || user[0] == '\0') user = std::getenv("USER");
    if (!user || user[0] == '\0') user = "unknown";
    const char* host = std::getenv("COMPUTERNAME");
    if (!host || host[0] == '\0') host = std::getenv("HOSTNAME");
    if (!host || host[0] == '\0') host = "unknown";
    return std::string(user) + "@" + host;
}

}  // namespace agent::audit
