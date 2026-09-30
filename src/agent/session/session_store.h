/**
 * @file session_store.h
 * @brief JSONL 会话存储（每条消息实时追加）
 * @details 参考 cc 的 .jsonl 格式，每行一个 JSON 事件。
 *          写入时 open + append，读取时逐行解析。
 *
 *          事件类型：
 *          - session_start：会话元信息（cwd/model/gitBranch/createdAt）
 *          - user：用户消息
 *          - assistant：助手消息（含 reasoning_content / tool_uses）
 *          - tool：工具结果消息
 *          - title：会话标题（append-only，/rename 追加新事件覆盖旧标题）
 *          - system_prompt：系统提示词快照（append-only，reason 区分 initial/changed/resume，
 *            含 hash 便于前端检测变更，调试会话轨迹用）
 *          - session_end：会话结束标记
 *
 *          存储路径：~/.workx/projects/<编码路径>/<sessionId>.jsonl
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>

#include "core/export.h"
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "agent/api/chat_types.h"
#include "core/todo/todo_item.h"  // #24：待办清单持久化

namespace agent::session {

/// @brief 子 Agent 持久化事件（progress/completed 统一结构，subType 字段区分）
/// @details 第二层（子 Agent 记录）持久化：ChatSession 订阅
///          SubAgentProgressEvent/SubAgentCompletedEvent 后转为本结构追加到 JSONL，
///          /resume 时按写入顺序重放恢复 sub_records（含观察合并语义）。
struct SubAgentEvent {
    std::string type;  ///< "progress" / "completed"
    std::string task_id;
    int32_t step_number = 0;
    std::string step_type;  ///< progress: "thought"/"action"/"observation"
    std::string content;
    std::string thought_text;
    std::string tool_name;
    std::string tool_input;
    std::string observation;
    bool is_error = false;
    double duration_ms = 0.0;
    std::string final_answer;  ///< completed
    bool was_error = false;    ///< completed
};

/// @brief 序列化到 JSON（SessionStore 持久化；外层 type 恒为 "sub_agent"）
void to_json(nlohmann::json& j, const SubAgentEvent& ev);

/// @brief 从 JSON 反序列化（缺省字段用默认值）
void from_json(const nlohmann::json& j, SubAgentEvent& ev);

/// @brief 手动调用技能持久化事件（UI 注入合成 Skill 卡片 /resume 时重建转录显示）
/// @details 由 handle_skill_invocation 落盘。query 记录实际发往模型的展开提示词，
///          /resume 重建转录区时按 query 匹配对应会话 user 消息，转为其"原始输入回显
///          + Skill 卡"。本事件仅影响转录区显示，不进入模型上下文。
struct SkillEvent {
    std::string name;       ///< 技能名（不含前导 /）
    std::string input;      ///< 用户输入的参数文本
    std::string raw_input;  ///< 用户完整原始输入（回显用）
    std::string query;  ///< 实际发往模型的展开提示词（用于恢复时定位对应 user 消息）
    bool is_error = false;  ///< 技能本地解析是否出错
};

/// @brief 序列化到 JSON（外层 type 恒为 "skill"）
void to_json(nlohmann::json& j, const SkillEvent& ev);

/// @brief 从 JSON 反序列化（缺省字段用默认值）
void from_json(const nlohmann::json& j, SkillEvent& ev);

/// @brief 会话元信息（用于列表展示）
struct SessionMeta {
    std::string session_id;  ///< 会话 ID
    std::string file_path;   ///< JSONL 文件路径
    std::string created_at;  ///< 创建时间（ISO 8601）
    std::string cwd;         ///< 会话工作目录
    std::string model;       ///< 模型名
    std::string git_branch;  ///< git 分支
    std::string title;       ///< 会话标题（最后一条 title 事件，无则 fallback）
    std::filesystem::file_time_type last_modified;  ///< 最后修改时间（用于排序）
    int message_count = 0;  ///< 消息数（不含 session_start/end/title）
};

/// @brief JSONL 会话存储（每条消息实时追加）
/// @details 写入时 open + append（flush 保证崩溃不丢），读取时逐行解析。
class WORKX_API SessionStore {
   public:
    /// @brief 构造
    /// @param file_path JSONL 文件路径
    /// @param session_id 会话 ID（写入 session_start/end 事件时使用）
    explicit SessionStore(std::string file_path, std::string session_id = "");

    ~SessionStore();

    /// @brief 打开文件（追加模式，不存在则创建）
    /// @return true=成功
    bool open();

    /// @brief 关闭文件
    void close();

    /// @brief 设置会话 ID（用于 session_start/end 事件）
    void set_session_id(std::string id) { m_session_id = std::move(id); }

    /// @brief 追加 session_start 事件
    bool append_session_start(const std::string& cwd, const std::string& model,
                              const std::string& git_branch);

    /// @brief 追加 user 消息
    bool append_user_message(const std::string& uuid, const std::string& parent_uuid,
                             const std::string& content, const std::string& timestamp);

    /// @brief 追加 assistant 消息
    bool append_assistant_message(const std::string& uuid, const std::string& parent_uuid,
                                  const std::string& content, const std::string& reasoning_content,
                                  const std::vector<ToolUse>& tool_uses,
                                  const std::string& timestamp, double reasoning_ms = 0.0);

    /// @brief 追加 tool 消息
    bool append_tool_message(const std::string& uuid, const std::string& parent_uuid,
                             const std::string& tool_call_id, const std::string& tool_name,
                             const std::string& content, bool is_error,
                             const std::string& timestamp);

    /// @brief 追加 session_end 事件
    bool append_session_end();

    /// @brief 追加 title 事件（/rename 或首条消息自动生成标题时调用）
    /// @details append-only：新 title 事件覆盖旧标题，读取时取最后一条
    bool append_title(const std::string& title);

    /// @brief 追加 system_prompt 事件（系统提示词快照，会话轨迹调试用）
    /// @param reason 记录原因：initial（会话初始）/ changed（运行时变更）/ resume（恢复会话）
    /// @param content 系统提示词全文
    /// @details append-only：每次变更追加一条快照（含 djb2 hash），前端据此
    ///          检测提示词是否变化并高亮 diff。
    bool append_system_prompt(const std::string& reason, const std::string& content);

    /// @brief 追加 todo 事件（#24：待办清单全量快照）
    /// @details append-only：每次变更追加一条完整快照，读取时取最后一条。
    ///          空列表也写入（表示清空），保证恢复语义正确。
    bool append_todo(const std::vector<core::todo::TodoItem>& todos);

    /// @brief 追加子 Agent 事件（progress/completed，第二层记录持久化）
    /// @details append-only：按事件发生顺序逐条追加，/resume 时按序重放恢复。
    bool append_sub_agent(const SubAgentEvent& ev);

    /// @brief 追加手动调用技能事件（合成 Skill 卡持久化）
    /// @details append-only：按发生顺序逐条追加，/resume 重建转录区时按 query 匹配恢复。
    bool append_skill(const SkillEvent& ev);

    // ============================================================
    // 静态工具方法
    // ============================================================

    /// @brief 读取 JSONL 文件所有事件
    static std::vector<nlohmann::json> read_all(const std::string& file_path);

    /// @brief 列出项目目录下的所有会话（按修改时间倒序）
    /// @param project_dir 项目目录路径
    /// @return 会话元信息列表
    static std::vector<SessionMeta> list_sessions(const std::string& project_dir);

    /// @brief 从 JSONL 文件加载消息历史（过滤掉 session_start/end 事件）
    /// @return ChatMessage 列表
    static std::vector<ChatMessage> load_messages(const std::string& file_path);

    /// @brief 从 JSONL 文件加载会话元信息
    static std::optional<SessionMeta> load_meta(const std::string& file_path);

    /// @brief 从 JSONL 文件加载待办清单（#24：取最后一条 todo 事件）
    /// @return 待办列表（无 todo 事件时返回空）
    static std::vector<core::todo::TodoItem> load_todos(const std::string& file_path);

    /// @brief 从 JSONL 文件加载子 Agent 事件（按写入顺序）
    /// @return 子 Agent 事件列表（无则空）
    static std::vector<SubAgentEvent> load_sub_agents(const std::string& file_path);

    /// @brief 从 JSONL 文件加载手动调用技能事件（按写入顺序）
    /// @return 技能事件列表（无则空）
    static std::vector<SkillEvent> load_skills(const std::string& file_path);

   private:
    std::string m_file_path;
    std::ofstream m_out;
    std::string m_session_id;

    bool append_line(const nlohmann::json& j);
};

/// @brief 获取项目会话目录路径
/// @param config_dir 配置根目录（如 ~/.workx）
/// @param cwd 项目工作目录
/// @return <config_dir>/projects/<编码路径>/
std::filesystem::path get_project_session_dir(const std::filesystem::path& config_dir,
                                              const std::string& cwd);

}  // namespace agent::session
