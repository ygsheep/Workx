/**
 * @file audit_logger.h
 * @brief 审计日志系统（#37）
 * @details 与运行日志（liblogger）分离的结构化审计日志，记录工具调用、安全事件、
 *          会话/Agent 生命周期。独立 JSONL 文件、append-only、不受 LogLevel 屏蔽。
 *
 * 设计要点：
 * - **独立文件**：audit.jsonl，与 app.log 分离，避免被 set_level(INFO) 屏蔽
 * - **结构化**：每行一个 JSON 对象，便于 jq 查询或程序化分析
 * - **不可屏蔽**：审计事件必须记录，不受 LogLevel 影响（安全合规要求）
 * - **脱敏**：记录输入/输出时经 SecretScanner 扫描，命中则脱敏
 * - **轮转**：按日期 + 大小双触发，保留最近 N 天
 * - **线程安全**：mutex 保护写入（同步写入，每次 log 落盘）
 * - **append-only**：以 std::ios::app 打开，工具无法篡改历史记录
 *
 * @version 1.0.1
 * @date 2026-08
 */

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent::audit {

/// @brief 审计事件严重级别
enum class Severity {
    Info,     ///< 一般信息（工具调用、会话生命周期）
    Warn,     ///< 警告（权限拒绝、沙盒降级、敏感文件访问）
    Critical  ///< 严重（危险命令、密钥检测、路径穿越）
};

/// @brief 审计事件类型
enum class EventType {
    // 工具生命周期
    ToolInvoke,              ///< 工具调用
    ToolPermissionDecision,  ///< 权限决策（allow/ask/deny）
    ToolPermissionDenied,    ///< 权限拒绝
    ToolResultTruncated,     ///< 输出被截断

    // 安全事件
    SecurityDangerousCommand,  ///< 危险命令检测
    SecuritySensitiveFile,     ///< 敏感文件访问
    SecuritySecretDetected,    ///< 密钥检测命中
    SecuritySandboxDisabled,   ///< 沙盒被关闭
    SecuritySandboxDegraded,   ///< 沙盒降级
    SecurityPathTraversal,     ///< 路径穿越尝试
    SecuritySymlinkBypass,     ///< 符号链接绕过尝试
    SecuritySSRFAttempt,       ///< SSRF 尝试
    SecurityEnvVarLeak,        ///< 环境变量泄露尝试

    // 会话/Agent 生命周期
    SessionStart,        ///< 会话开始
    SessionEnd,          ///< 会话结束
    AgentStart,          ///< Agent 开始
    AgentDone,           ///< Agent 完成
    AgentInterrupted,    ///< Agent 被取消
    AgentMaxIterations,  ///< 达到最大迭代
};

/// @brief 将 EventType 转换为字符串标识（用于 JSONL event_type 字段）
std::string_view to_string(EventType type) noexcept;

/// @brief 将 Severity 转换为字符串（用于 JSONL severity 字段）
std::string_view to_string(Severity sev) noexcept;

/// @brief 审计事件（构建后传给 AuditLogger::log）
struct AuditEvent {
    EventType type;               ///< 事件类型
    Severity severity;            ///< 严重级别
    std::string session_id;       ///< 会话 ID（可选）
    std::string request_id;       ///< 请求 ID（可选）
    std::string trace_id;         ///< 调用链 ID（可选，如 "root"、"parent:child"）
    std::string tool_name;        ///< 工具名称（工具事件必填）
    nlohmann::json input;         ///< 工具输入关键字段（非全量，避免日志爆炸）
    std::string decision;         ///< 权限决策结果（allow/ask/deny）
    std::string decision_reason;  ///< 决策原因（哪条规则触发）
    int64_t duration_ms = 0;      ///< 耗时（毫秒）
    std::string output_summary;   ///< 输出摘要（前 200 字符）
    std::vector<std::string> security_flags;  ///< 安全标记（如 "destructive_command"）
    std::string user;                         ///< 用户@主机
};

/// @brief 审计日志记录器（单例）
/// @details 独立于 liblogger 的运行日志，专用于安全审计。
///          线程安全，同步写入，不可屏蔽。
class AuditLogger {
   public:
    /// @brief 获取单例实例
    static AuditLogger& instance() noexcept;

    /// @brief 初始化审计日志文件
    /// @param file_path 审计日志文件路径（如 ~/.workx/logs/audit.jsonl）
    /// @param max_size_mb 单文件大小上限（MB），超过时触发轮转
    /// @param retention_days 保留天数
    void init(const std::string& file_path, size_t max_size_mb = 10, size_t retention_days = 30);

    /// @brief 启用/禁用审计日志
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }

    /// @brief 检查是否已启用
    bool is_enabled() const noexcept { return enabled_; }

    /// @brief 记录审计事件（核心方法）
    /// @details 将事件序列化为 JSONL 并同步写入文件。
    ///          输入/输出字段经脱敏处理后记录。
    ///          若未启用则 no-op。
    void log(AuditEvent event);

    // ==================== 便捷方法 ====================

    /// @brief 记录工具调用
    /// @param tool_name 工具名称
    /// @param input 工具输入（将脱敏后记录关键字段）
    /// @param session_id 会话 ID
    /// @param request_id 请求 ID
    /// @param decision 权限决策（"allow"/"ask"/"deny"）
    /// @param decision_reason 决策原因
    /// @param duration_ms 耗时
    /// @param output 输出文本（将脱敏后取前 200 字符）
    /// @param security_flags 安全标记
    void log_tool_invoke(const std::string& tool_name, const nlohmann::json& input,
                         const std::string& session_id, const std::string& request_id,
                         std::string_view decision = "allow", std::string_view decision_reason = "",
                         int64_t duration_ms = 0, std::string_view output = "",
                         const std::vector<std::string>& security_flags = {});

    /// @brief 记录安全违规事件
    /// @param type 事件类型（须为 Security* 系列）
    /// @param detail 事件详情
    /// @param session_id 会话 ID
    /// @param tool_name 相关工具名称（可选）
    void log_security(EventType type, const std::string& detail, const std::string& session_id,
                      std::string_view tool_name = "");

    /// @brief 记录会话生命周期事件
    void log_session_lifecycle(EventType type, const std::string& session_id);

    /// @brief 记录 Agent 生命周期事件
    void log_agent_lifecycle(EventType type, const std::string& session_id, int iteration = 0);

    /// @brief 手动 flush（用于测试和优雅关闭）
    void flush();

   private:
    AuditLogger() = default;
    ~AuditLogger();
    AuditLogger(const AuditLogger&) = delete;
    AuditLogger& operator=(const AuditLogger&) = delete;

    /// @brief 序列化事件为 JSONL 行
    std::string serialize(const AuditEvent& event) const;

    /// @brief 脱敏处理：扫描文本中的密钥，命中则替换为 [REDACTED:label]
    std::string redact(const std::string& text) const;

    /// @brief 脱敏 JSON 输入：提取关键字段并脱敏
    nlohmann::json redact_input(const nlohmann::json& input) const;

    /// @brief 检查并执行轮转
    void maybe_rotate();

    /// @brief 清理过期日志文件
    void cleanup_expired();

    /// @brief 获取当前时间戳（ISO 8601 带时区）
    std::string timestamp_now() const;

    /// @brief 获取当前用户@主机（USERNAME/USER + COMPUTERNAME/HOSTNAME）
    std::string current_user_host() const;

    std::mutex mutex_;
    std::string file_path_;
    size_t max_size_bytes_ = 10 * 1024 * 1024;  ///< 10 MB
    size_t retention_days_ = 30;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> initialized_{false};
};

}  // namespace agent::audit
