/**
 * @file test_audit_logger.cpp
 * @brief AuditLogger 单元测试（#37）
 * @details 覆盖 JSONL 格式、脱敏、轮转、事件类型、便捷方法
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

#include "agent/audit/audit_logger.h"

using namespace agent::audit;
namespace fs = std::filesystem;

namespace {

/// @brief 读取文件全部内容
std::string read_file(const fs::path& path) {
    std::ifstream ifs(path);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return content;
}

/// @brief 读取 JSONL 文件并解析为 JSON 数组
std::vector<nlohmann::json> read_jsonl(const fs::path& path) {
    std::vector<nlohmann::json> lines;
    std::ifstream ifs(path);
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        lines.push_back(nlohmann::json::parse(line));
    }
    return lines;
}

/// @brief 临时审计日志测试 fixture
class AuditFixture {
   public:
    fs::path temp_dir;
    fs::path audit_file;

    AuditFixture() {
        temp_dir = fs::temp_directory_path() /
                   ("workx_audit_test_" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(temp_dir);
        audit_file = temp_dir / "audit.jsonl";
    }

    ~AuditFixture() {
        std::error_code ec;
        fs::remove_all(temp_dir, ec);
    }

    void init_logger(size_t max_size_mb = 10, size_t retention_days = 30) {
        AuditLogger::instance().init(audit_file.string(), max_size_mb, retention_days);
    }
};

}  // anonymous namespace

// ============================================================
// 1. JSONL 格式与基本写入
// ============================================================

TEST_CASE("AuditLogger writes valid JSONL", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", "ls -la"}},
                                            "session-123", "req-456", "allow", "", 150, "total 10",
                                            {});

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 1);

    auto& j = lines[0];
    REQUIRE(j["event_type"] == "tool.invoke");
    REQUIRE(j["severity"] == "info");
    REQUIRE(j["session_id"] == "session-123");
    REQUIRE(j["request_id"] == "req-456");
    REQUIRE(j["tool_name"] == "BashTool");
    REQUIRE(j["decision"] == "allow");
    REQUIRE(j["duration_ms"] == 150);
    REQUIRE(j["output_summary"] == "total 10");
    REQUIRE(j["input"]["command"] == "ls -la");
    REQUIRE(j["input"]["_size"] > 0);
    REQUIRE(j.contains("timestamp"));
    REQUIRE(j.contains("user"));
}

// ============================================================
// 2. 脱敏：密钥不进入审计日志
// ============================================================

TEST_CASE("AuditLogger redacts secrets in output", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    // 构造含 AWS key 的输出
    std::string secret_output = "AKIAIOSFODNN7EXAMPLE\nwJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";

    AuditLogger::instance().log_tool_invoke("BashTool",
                                            nlohmann::json{{"command", "cat ~/.aws/credentials"}},
                                            "sess-1", "req-1", "allow", "", 50, secret_output, {});

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 1);

    std::string output = lines[0]["output_summary"];
    // 原始密钥不应出现在日志中
    REQUIRE(output.find("AKIAIOSFODNN7EXAMPLE") == std::string::npos);
    // 应包含 REDACTED 标记
    REQUIRE(output.find("REDACTED") != std::string::npos);
}

TEST_CASE("AuditLogger redacts secrets in input fields", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    // 构造含 GitHub PAT 的 command 字段
    std::string secret_cmd =
        "curl -H 'Authorization: token ghp_1234567890abcdefghijklmnopqrstuvwxyz' "
        "https://api.github.com";

    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", secret_cmd}},
                                            "sess-1", "req-1", "allow", "", 10, "", {});

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 1);

    std::string cmd = lines[0]["input"]["command"];
    // 原始 PAT 不应出现在日志中
    REQUIRE(cmd.find("ghp_1234567890abcdefghijklmnopqrstuvwxyz") == std::string::npos);
    REQUIRE(cmd.find("REDACTED") != std::string::npos);
}

// ============================================================
// 3. 安全标记提升 severity
// ============================================================

TEST_CASE("AuditLogger security flags elevate severity", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().log_tool_invoke(
        "BashTool", nlohmann::json{{"command", "rm -rf /"}}, "sess-1", "req-1", "deny",
        "destructive command detected", 0, "", {"destructive_command"});

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 1);

    REQUIRE(lines[0]["severity"] == "warn");
    REQUIRE(lines[0]["decision"] == "deny");
    REQUIRE(lines[0]["security_flags"][0] == "destructive_command");
}

// ============================================================
// 4. 便捷方法：安全事件
// ============================================================

TEST_CASE("AuditLogger log_security records critical events", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().log_security(EventType::SecurityDangerousCommand,
                                         "rm -rf / detected and blocked", "sess-1", "BashTool");

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 1);

    REQUIRE(lines[0]["event_type"] == "security.dangerous_command");
    REQUIRE(lines[0]["severity"] == "critical");
    REQUIRE(lines[0]["tool_name"] == "BashTool");
    REQUIRE(lines[0]["input"]["detail"] == "rm -rf / detected and blocked");
}

// ============================================================
// 5. 便捷方法：会话/Agent 生命周期
// ============================================================

TEST_CASE("AuditLogger session lifecycle", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().log_session_lifecycle(EventType::SessionStart, "sess-1");
    AuditLogger::instance().log_session_lifecycle(EventType::SessionEnd, "sess-1");

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 2);

    REQUIRE(lines[0]["event_type"] == "session.start");
    REQUIRE(lines[0]["severity"] == "info");
    REQUIRE(lines[1]["event_type"] == "session.end");
}

TEST_CASE("AuditLogger agent lifecycle with iteration", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().log_agent_lifecycle(EventType::AgentStart, "sess-1");
    AuditLogger::instance().log_agent_lifecycle(EventType::AgentMaxIterations, "sess-1", 25);

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 2);

    REQUIRE(lines[0]["event_type"] == "agent.start");
    REQUIRE(lines[0]["severity"] == "info");

    REQUIRE(lines[1]["event_type"] == "agent.max_iterations");
    REQUIRE(lines[1]["severity"] == "warn");
    REQUIRE(lines[1]["input"]["iteration"] == 25);
}

// ============================================================
// 6. 轮转：超过大小上限时触发
// ============================================================

TEST_CASE("AuditLogger rotates on size limit", "[audit]") {
    AuditFixture fix;
    // 设置 1MB 上限（init 接口以 MB 为单位）
    fix.init_logger(1, 30);

    // 写入一个事件建立基线
    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", "echo x"}},
                                            "sess-1", "req-0", "allow", "", 10, "output", {});

    // 直接填充文件超过上限（模拟积累的日志，避免写入 50+ 次事件拖慢测试）
    {
        std::ofstream ofs(fix.audit_file, std::ios::app);
        ofs << std::string(1024 * 1024 + 512, 'X') << '\n';
    }

    // 再写一个事件触发轮转
    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", "echo y"}},
                                            "sess-1", "req-1", "allow", "", 10, "output", {});

    // 轮转后重建：再写一个事件，原文件应重新出现
    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", "echo z"}},
                                            "sess-1", "req-2", "allow", "", 10, "output", {});

    // 原文件应存在（轮转后重建）
    REQUIRE(fs::exists(fix.audit_file));

    // 应至少有一个轮转文件
    bool has_rotated = false;
    for (const auto& entry : fs::directory_iterator(fix.temp_dir)) {
        auto name = entry.path().filename().string();
        if (name.find("audit.") == 0 && name != "audit.jsonl" &&
            name.find(".jsonl") != std::string::npos) {
            has_rotated = true;
            break;
        }
    }
    REQUIRE(has_rotated);
}

// ============================================================
// 7. 禁用时不写入
// ============================================================

TEST_CASE("AuditLogger disabled produces no output", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().set_enabled(false);

    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", "ls"}}, "sess-1",
                                            "req-1", "allow", "", 5, "output", {});

    AuditLogger::instance().set_enabled(true);

    // 文件应为空或不存在
    if (fs::exists(fix.audit_file)) {
        REQUIRE(read_file(fix.audit_file).empty());
    }
}

// ============================================================
// 8. input 提取关键字段
// ============================================================

TEST_CASE("AuditLogger extracts key input fields", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    nlohmann::json input = {{"file_path", "/etc/passwd"},
                            {"command", "cat /etc/passwd"},
                            {"unknown_field", "should not appear"},
                            {"nested", {{"deep", "value"}}}};

    AuditLogger::instance().log_tool_invoke("BashTool", input, "sess-1", "req-1", "allow", "", 5,
                                            "", {});

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 1);

    auto& recorded_input = lines[0]["input"];
    // 关键字段应被提取
    REQUIRE(recorded_input.contains("file_path"));
    REQUIRE(recorded_input.contains("command"));
    REQUIRE(recorded_input.contains("_size"));
    // 非关键字段不应出现
    REQUIRE_FALSE(recorded_input.contains("unknown_field"));
    REQUIRE_FALSE(recorded_input.contains("nested"));
}

// ============================================================
// 9. EventType 字符串映射
// ============================================================

TEST_CASE("EventType string mapping", "[audit]") {
    REQUIRE(to_string(EventType::ToolInvoke) == "tool.invoke");
    REQUIRE(to_string(EventType::SecurityDangerousCommand) == "security.dangerous_command");
    REQUIRE(to_string(EventType::SecuritySecretDetected) == "security.secret_detected");
    REQUIRE(to_string(EventType::SessionStart) == "session.start");
    REQUIRE(to_string(EventType::AgentMaxIterations) == "agent.max_iterations");
}

TEST_CASE("Severity string mapping", "[audit]") {
    REQUIRE(to_string(Severity::Info) == "info");
    REQUIRE(to_string(Severity::Warn) == "warn");
    REQUIRE(to_string(Severity::Critical) == "critical");
}

// ============================================================
// 10. 多事件追加（append-only）
// ============================================================

TEST_CASE("AuditLogger appends multiple events", "[audit]") {
    AuditFixture fix;
    fix.init_logger();

    AuditLogger::instance().log_session_lifecycle(EventType::SessionStart, "sess-1");
    AuditLogger::instance().log_tool_invoke("FileReadTool",
                                            nlohmann::json{{"file_path", "/tmp/test"}}, "sess-1",
                                            "req-1", "allow", "", 10, "content", {});
    AuditLogger::instance().log_tool_invoke("BashTool", nlohmann::json{{"command", "ls"}}, "sess-1",
                                            "req-2", "allow", "", 5, "file1\nfile2", {});
    AuditLogger::instance().log_session_lifecycle(EventType::SessionEnd, "sess-1");

    auto lines = read_jsonl(fix.audit_file);
    REQUIRE(lines.size() == 4);

    REQUIRE(lines[0]["event_type"] == "session.start");
    REQUIRE(lines[1]["event_type"] == "tool.invoke");
    REQUIRE(lines[1]["tool_name"] == "FileReadTool");
    REQUIRE(lines[2]["event_type"] == "tool.invoke");
    REQUIRE(lines[2]["tool_name"] == "BashTool");
    REQUIRE(lines[3]["event_type"] == "session.end");
}
