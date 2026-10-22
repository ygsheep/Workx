/**
 * @file session_store.cpp
 * @brief JSONL 会话存储实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/session/session_store.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>

#include "core/utils/path_encoder.h"

namespace agent::session {

namespace {

/// @brief 获取当前 ISO 8601 时间戳（UTC）
std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

/// @brief 稳定字符串 hash（djb2），用于检测系统提示词是否变化
std::string djb2_hex(const std::string& s) {
    unsigned long h = 5381;
    for (unsigned char c : s) h = h * 33 + c;
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << h;
    return oss.str();
}

}  // anonymous namespace

// ============================================================
// SubAgentEvent 序列化
// ============================================================

void to_json(nlohmann::json& j, const SubAgentEvent& ev) {
    j = nlohmann::json{
        {"type", "sub_agent"},
        {"subType", ev.type},
        {"taskId", ev.task_id},
        {"stepNumber", ev.step_number},
        {"stepType", ev.step_type},
        {"content", ev.content},
        {"thoughtText", ev.thought_text},
        {"toolName", ev.tool_name},
        {"toolInput", ev.tool_input},
        {"observation", ev.observation},
        {"isError", ev.is_error},
        {"durationMs", ev.duration_ms},
        {"finalAnswer", ev.final_answer},
        {"wasError", ev.was_error},
    };
}

void from_json(const nlohmann::json& j, SubAgentEvent& ev) {
    ev.type = j.value("subType", std::string{});
    ev.task_id = j.value("taskId", std::string{});
    ev.step_number = j.value("stepNumber", 0);
    ev.step_type = j.value("stepType", std::string{});
    ev.content = j.value("content", std::string{});
    ev.thought_text = j.value("thoughtText", std::string{});
    ev.tool_name = j.value("toolName", std::string{});
    ev.tool_input = j.value("toolInput", std::string{});
    ev.observation = j.value("observation", std::string{});
    ev.is_error = j.value("isError", false);
    ev.duration_ms = j.value("durationMs", 0.0);
    ev.final_answer = j.value("finalAnswer", std::string{});
    ev.was_error = j.value("wasError", false);
}

// ============================================================
// SkillEvent 序列化
// ============================================================

void to_json(nlohmann::json& j, const SkillEvent& ev) {
    j = nlohmann::json{
        {"type", "skill"},          {"name", ev.name},   {"input", ev.input},
        {"rawInput", ev.raw_input}, {"query", ev.query}, {"isError", ev.is_error},
    };
}

void from_json(const nlohmann::json& j, SkillEvent& ev) {
    ev.name = j.value("name", std::string{});
    ev.input = j.value("input", std::string{});
    ev.raw_input = j.value("rawInput", std::string{});
    ev.query = j.value("query", std::string{});
    ev.is_error = j.value("isError", false);
}

// ============================================================
// PermissionEvent 序列化（#87）
// ============================================================

void to_json(nlohmann::json& j, const PermissionEvent& ev) {
    j = nlohmann::json{
        {"type", "permission"},
        {"permissionMode", ev.permission_mode},
        {"sessionMode", ev.session_mode},
        {"permissionModeBeforePlan", ev.permission_mode_before_plan},
        {"inPlan", ev.in_plan},
    };
}

void from_json(const nlohmann::json& j, PermissionEvent& ev) {
    ev.permission_mode = j.value("permissionMode", std::string{});
    ev.session_mode = j.value("sessionMode", std::string{});
    ev.permission_mode_before_plan = j.value("permissionModeBeforePlan", std::string{});
    ev.in_plan = j.value("inPlan", false);
}

// ============================================================
// 生命周期
// ============================================================

SessionStore::SessionStore(std::string file_path, std::string session_id)
    : m_file_path(std::move(file_path)), m_session_id(std::move(session_id)) {}

SessionStore::~SessionStore() { close(); }

bool SessionStore::open() {
    // 确保父目录存在
    auto parent = std::filesystem::path(m_file_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    // 追加模式打开（不截断已有内容）
    m_out.open(m_file_path, std::ios::app);
    return m_out.is_open();
}

void SessionStore::close() {
    if (m_out.is_open()) {
        m_out.flush();
        m_out.close();
    }
}

bool SessionStore::append_line(const nlohmann::json& j) {
    if (!m_out.is_open()) return false;
    m_out << j.dump() << "\n";
    m_out.flush();  // 实时刷盘，崩溃不丢消息
    return m_out.good();
}

// ============================================================
// 事件追加
// ============================================================

bool SessionStore::append_session_start(const std::string& cwd, const std::string& model,
                                        const std::string& git_branch) {
    nlohmann::json j;
    j["type"] = "session_start";
    j["sessionId"] = m_session_id;
    j["cwd"] = cwd;
    j["model"] = model;
    j["gitBranch"] = git_branch;
    j["createdAt"] = now_iso();
    return append_line(j);
}

bool SessionStore::append_user_message(const std::string& uuid, const std::string& parent_uuid,
                                       const std::string& content, const std::string& timestamp) {
    nlohmann::json j;
    j["type"] = "user";
    j["uuid"] = uuid;
    j["parentUuid"] = parent_uuid;
    j["timestamp"] = timestamp;
    j["content"] = content;
    return append_line(j);
}

bool SessionStore::append_assistant_message(const std::string& uuid, const std::string& parent_uuid,
                                            const std::string& content,
                                            const std::string& reasoning_content,
                                            const std::vector<ToolUse>& tool_uses,
                                            const std::string& timestamp, double reasoning_ms) {
    nlohmann::json j;
    j["type"] = "assistant";
    j["uuid"] = uuid;
    j["parentUuid"] = parent_uuid;
    j["timestamp"] = timestamp;
    j["content"] = content;
    j["reasoningContent"] = reasoning_content;
    j["reasoningMs"] = reasoning_ms;
    nlohmann::json uses = nlohmann::json::array();
    for (const auto& u : tool_uses) {
        uses.push_back({{"id", u.id}, {"name", u.name}, {"input", u.input}});
    }
    j["toolUses"] = uses;
    return append_line(j);
}

bool SessionStore::append_tool_message(const std::string& uuid, const std::string& parent_uuid,
                                       const std::string& tool_call_id,
                                       const std::string& tool_name, const std::string& content,
                                       bool is_error, const std::string& timestamp) {
    nlohmann::json j;
    j["type"] = "tool";
    j["uuid"] = uuid;
    j["parentUuid"] = parent_uuid;
    j["timestamp"] = timestamp;
    j["toolCallId"] = tool_call_id;
    j["toolName"] = tool_name;
    j["content"] = content;
    j["isError"] = is_error;
    return append_line(j);
}

bool SessionStore::append_session_end() {
    nlohmann::json j;
    j["type"] = "session_end";
    j["sessionId"] = m_session_id;
    j["endedAt"] = now_iso();
    return append_line(j);
}

bool SessionStore::append_title(const std::string& title) {
    nlohmann::json j;
    j["type"] = "title";
    j["sessionId"] = m_session_id;
    j["timestamp"] = now_iso();
    j["title"] = title;
    return append_line(j);
}

bool SessionStore::append_system_prompt(const std::string& reason, const std::string& content) {
    nlohmann::json j;
    j["type"] = "system_prompt";
    j["sessionId"] = m_session_id;
    j["timestamp"] = now_iso();
    j["reason"] = reason;
    j["content"] = content;
    j["hash"] = djb2_hex(content);
    return append_line(j);
}

bool SessionStore::append_todo(const std::vector<core::todo::TodoItem>& todos) {
    nlohmann::json j;
    j["type"] = "todo";
    j["sessionId"] = m_session_id;
    j["timestamp"] = now_iso();
    j["todos"] = todos;
    return append_line(j);
}

bool SessionStore::append_sub_agent(const SubAgentEvent& ev) {
    nlohmann::json j = ev;
    j["sessionId"] = m_session_id;
    j["timestamp"] = now_iso();
    return append_line(j);
}

bool SessionStore::append_skill(const SkillEvent& ev) {
    nlohmann::json j = ev;
    j["sessionId"] = m_session_id;
    j["timestamp"] = now_iso();
    return append_line(j);
}

bool SessionStore::append_permission(const PermissionEvent& ev) {
    nlohmann::json j = ev;
    j["sessionId"] = m_session_id;
    j["timestamp"] = now_iso();
    return append_line(j);
}

// ============================================================
// 静态方法
// ============================================================

std::vector<nlohmann::json> SessionStore::read_all(const std::string& file_path) {
    std::vector<nlohmann::json> events;
    std::ifstream in(file_path);
    if (!in.is_open()) return events;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            events.push_back(nlohmann::json::parse(line));
        } catch (const std::exception&) {
            // 跳过损坏的行（可能是崩溃时写了一半）
            continue;
        }
    }
    return events;
}

std::vector<SessionMeta> SessionStore::list_sessions(const std::string& project_dir) {
    std::vector<SessionMeta> sessions;
    std::error_code ec;
    if (!std::filesystem::exists(project_dir, ec)) return sessions;

    for (const auto& entry : std::filesystem::directory_iterator(project_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".jsonl") continue;

        auto meta = load_meta(entry.path().string());
        if (meta) {
            meta->file_path = entry.path().string();
            meta->last_modified = entry.last_write_time(ec);
            sessions.push_back(*meta);
        }
    }

    // 按修改时间倒序（最新在前）
    std::sort(sessions.begin(), sessions.end(), [](const SessionMeta& a, const SessionMeta& b) {
        return a.last_modified > b.last_modified;
    });
    return sessions;
}

std::vector<ChatMessage> SessionStore::load_messages(const std::string& file_path) {
    std::vector<ChatMessage> messages;
    auto events = read_all(file_path);

    for (const auto& j : events) {
        std::string type = j.value("type", "");
        if (type == "user") {
            ChatMessage msg = ChatMessage::user(j.value("content", ""));
            messages.push_back(msg);
        } else if (type == "assistant") {
            ChatMessage msg = ChatMessage::assistant(j.value("content", ""));
            msg.reasoning_content = j.value("reasoningContent", "");
            msg.reasoning_ms = j.value("reasoningMs", 0.0);
            if (j.contains("toolUses") && j["toolUses"].is_array()) {
                for (const auto& u : j["toolUses"]) {
                    ToolUse tu;
                    tu.id = u.value("id", "");
                    tu.name = u.value("name", "");
                    tu.input = u.value("input", nlohmann::json::object());
                    msg.tool_uses.push_back(tu);
                }
            }
            messages.push_back(msg);
        } else if (type == "tool") {
            ChatMessage msg =
                ChatMessage::tool_result(j.value("toolCallId", ""), j.value("toolName", ""),
                                         j.value("content", ""), j.value("isError", false));
            messages.push_back(msg);
        }
        // session_start / session_end 不转为消息
    }
    return messages;
}

std::optional<SessionMeta> SessionStore::load_meta(const std::string& file_path) {
    auto events = read_all(file_path);
    if (events.empty()) return std::nullopt;

    SessionMeta meta;
    int msg_count = 0;
    std::string first_user_content;  // fallback 标题来源
    bool has_title = false;

    for (const auto& j : events) {
        std::string type = j.value("type", "");
        if (type == "session_start") {
            meta.session_id = j.value("sessionId", "");
            meta.cwd = j.value("cwd", "");
            meta.model = j.value("model", "");
            meta.git_branch = j.value("gitBranch", "");
            meta.created_at = j.value("createdAt", "");
        } else if (type == "title") {
            // 取最后一条 title 事件作为当前标题
            meta.title = j.value("title", "");
            has_title = true;
        } else if (type == "user" || type == "assistant" || type == "tool") {
            ++msg_count;
            // 记录首条 user 消息内容用于 fallback
            if (type == "user" && first_user_content.empty()) {
                first_user_content = j.value("content", "");
            }
        }
    }
    meta.message_count = msg_count;

    // 标题 fallback：无 title 事件时取首条 user 消息前 20 字
    if (!has_title) {
        if (!first_user_content.empty()) {
            // 截取前 20 字（按 UTF-8 字符数，避免截断多字节字符）
            size_t char_count = 0;
            size_t byte_pos = 0;
            while (char_count < 20 && byte_pos < first_user_content.size()) {
                unsigned char c = static_cast<unsigned char>(first_user_content[byte_pos]);
                if (c < 0x80)
                    byte_pos += 1;
                else if ((c & 0xE0) == 0xC0)
                    byte_pos += 2;
                else if ((c & 0xF0) == 0xE0)
                    byte_pos += 3;
                else if ((c & 0xF8) == 0xF0)
                    byte_pos += 4;
                else
                    byte_pos += 1;  // 无效 UTF-8，单字节前进
                ++char_count;
            }
            meta.title = first_user_content.substr(0, byte_pos);
            if (byte_pos < first_user_content.size()) {
                meta.title += "...";
            }
        } else {
            meta.title = "未命名会话";
        }
    }

    if (meta.session_id.empty()) {
        // 没有 session_start 事件，用文件名作为 session_id
        meta.session_id = std::filesystem::path(file_path).stem().string();
    }

    return meta;
}

std::vector<core::todo::TodoItem> SessionStore::load_todos(const std::string& file_path) {
    auto events = read_all(file_path);
    std::vector<core::todo::TodoItem> todos;
    for (const auto& j : events) {
        if (j.value("type", "") != "todo") continue;
        todos.clear();  // 取最后一条 todo 事件（append-only 快照）
        if (j.contains("todos") && j["todos"].is_array()) {
            for (const auto& t : j["todos"]) {
                todos.push_back(t.get<core::todo::TodoItem>());
            }
        }
    }
    return todos;
}

std::optional<PermissionEvent> SessionStore::load_permission(const std::string& file_path) {
    std::optional<PermissionEvent> latest;
    for (const auto& j : read_all(file_path)) {
        if (j.value("type", "") != "permission") continue;
        PermissionEvent ev = j.get<PermissionEvent>();  // 取最后一条（append-only 快照）
        latest = ev;
    }
    return latest;
}

std::vector<SubAgentEvent> SessionStore::load_sub_agents(const std::string& file_path) {
    std::vector<SubAgentEvent> events;
    for (const auto& j : read_all(file_path)) {
        if (j.value("type", "") != "sub_agent") continue;
        events.push_back(j.get<SubAgentEvent>());
    }
    return events;
}

std::vector<SkillEvent> SessionStore::load_skills(const std::string& file_path) {
    std::vector<SkillEvent> events;
    for (const auto& j : read_all(file_path)) {
        if (j.value("type", "") != "skill") continue;
        events.push_back(j.get<SkillEvent>());
    }
    return events;
}

std::filesystem::path get_project_session_dir(const std::filesystem::path& config_dir,
                                              const std::string& cwd) {
    return config_dir / "projects" / core::util::encode_project_path(cwd);
}

}  // namespace agent::session
