/**
 * @file action.h
 * @brief TuiAction — 后台事件回调 → UI 线程的不可变动作
 * @details EventBridge 订阅 EventBus 后只入队轻量的 action，不持锁碰 UI 状态；
 *          UI 线程每帧 drain 后应用到 ViewModel。见
 *          docs/plans/2026-08-17-ftxui-tui-design.md §3。
 * @version 0.1.0（实验）
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include "agent/api/backend_types.h"  // agent::ModelInfo（模型列表携带 context_length）
#include "agent/api/i_completion_provider.h"
#include "agent/model/provider_config.h"
#include "core/events/agent_events.h"  // agent::AskUserResult
#include "core/todo/todo_item.h"       // #24：待办清单条目（TodoUpdatedEvent 载荷）

namespace ftxtui {

/// @brief 追加一条已完成的消息（用户/历史回放/本地命令回显）
struct ActionAppendMessage {
    std::string role;  ///< "user" / "assistant"
    std::string text;
};

/// @brief 手动调用技能：注入合成 Skill 卡片（本地解析完成态）
/// @details 用户在消息任意位置输入 /skill-name 时，UI 回显原始输入后注入此卡，
///          以复用现有「Skills：名」工具卡渲染。卡片仅存在于 ViewModel 转录区，
///          不进入会话模型上下文（实际发往模型的仍是技能展开后的提示词）。
struct ActionAppendSkill {
    std::string name;       ///< 技能名（不含前导 /）
    std::string input;      ///< 用户传入的参数文本（卡内展示）
    bool is_error = false;  ///< 技能本地解析是否出错
};

/// @brief 本地命令执行完成：合成 Bash 卡片（！命令，跨平台 cmd/sh）
/// @details Enter 触发：仅执行并展示结果卡；卡片复用现有 Shell 工具卡渲染
///          （tool_name="Bash"，arguments 含 command，result 用 <stdout>/<stderr>/
///          <exit_code> 或 <error> 标签）。卡片只存在于 ViewModel 转录区。
struct ActionAppendCmdResult {
    std::string command;  ///< 命令文本（卡 subject 展示）
    std::string result;  ///< 已组装好标签文本（<stdout>/<stderr>/<exit_code> 或 <error>）
    bool is_error = false;  ///< 执行/退出码非 0 是否视为失败（默认展开）
};

/// @brief 把结构化命令结果作为用户消息提交给模型（！命令 + Ctrl+Enter）
/// @details UI 线程处理：回显 user 消息 + 置 busy + on_submit（文本已含命令、
///          退出码与输出，便于模型理解上下文）。
struct ActionSubmitCmdToModel {
    std::string text;  ///< 结构化文本（用户执行了什么命令、结果如何）
};

/// @brief 流式正文增量（追加到当前流式消息节点）
struct ActionTokenDelta {
    std::string content_delta;
};

/// @brief 流式思考增量（追加到当前流式消息的 reasoning 缓冲）
struct ActionReasoningDelta {
    std::string delta;
};

/// @brief ReAct 单步 LLM 输出结束（仍有工具待执行，轻量收尾）
struct ActionStepDone {};

/// @brief 整轮结束（提交当前流式消息，token 统计，状态回 IDLE）
struct ActionTurnDone {
    std::string full_content;
    std::string full_reasoning;
    bool was_interrupted = false;
    bool is_local_command = false;
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    int32_t cache_read_input_tokens = 0;
    // #65：DeepSeek 硬盘缓存命中/未命中（Anthropic adapter 留 0）
    int32_t prompt_cache_hit_tokens = 0;
    int32_t prompt_cache_miss_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
    double reasoning_ms = 0.0;  ///< 思考阶段实际耗时（毫秒，思考折叠标签显示）
};

/// @brief 推理错误
struct ActionError {
    std::string message;
};

/// @brief 工具调用开始（创建可折叠工具块）
struct ActionBeginTool {
    std::string tool_name;
    std::string call_id;
    std::string arguments;  ///< JSON 字符串
};

/// @brief 工具结果回传（写入工具块，可展开）
struct ActionEndTool {
    std::string call_id;
    std::string result;
    bool is_error = false;
};

/// @brief Agent 编排完成（汇总工具次数/耗时/最终答复）
/// @details 携带 final_answer，若当前没有流式消息（例如纯工具调用后直接结束），
///          用它兜底创建一条 assistant 消息。
struct ActionAgentDone {
    std::string final_answer;
    int32_t total_steps = 0;
    int32_t total_tool_calls = 0;
    double total_duration_ms = 0.0;
};

/// @brief 忙碌状态（后台是否有生成任务）
struct ActionSetBusy {
    bool busy = false;
};

/// @brief 权限模式标签更新
struct ActionPermissions {
    std::string label;  ///< "" / "bypass"
};

/// @brief 工作模式标签更新（标准 / 计划 / 极简）
/// @details 由 TUI 模式切换键、EnterPlanModeEvent/ExitPlanModeEvent（事件桥）
///          与 ActionPermissions 解耦：模式为顶层选择，权限在模式内独立切换。
struct ActionSetMode {
    std::string label;  ///< "standard" / "plan" / "minimal"
};

/// @brief AskUser 模态请求（携带回填 promise）
struct ActionAskUser {
    nlohmann::json questions;
    int32_t timeout_ms = 0;
    std::shared_ptr<std::promise<agent::AskUserResult>> result_promise;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

/// @brief AskUser 超时（关闭模态，返回 cancelled）
struct ActionAskUserTimeout {};

/// @brief 打开方案 markdown 预览（PlanPreviewEvent → 侧边栏文件 tab）
/// @details 退出规划模式时在侧边栏 /view 打开方案文件，替代在提问里直接展示全文。
struct ActionOpenPlan {
    std::string plan_path;  ///< 方案文件绝对路径（已落盘）
};

/// @brief 缓存诊断事件（DeepSeek 缓存命中率劣化归因）
/// @details prefix_changed=true 时提示缓存前缀变化原因，辅助理解命中率下降。
struct ActionCacheDiagnostics {
    std::string prefix_hash;
    bool prefix_changed = false;
    std::vector<std::string> reasons;  ///< 变化原因（"system"/"tools"/"log_rewrite"）
    int32_t cache_hit_tokens = 0;
    int32_t cache_miss_tokens = 0;
};

/// @brief 压缩暂停/恢复事件（DS_CACHE H-3 卡死守卫）
struct ActionCompactionPaused {
    bool paused = true;  ///< true=守卫触发暂停；false=自愈恢复
    int32_t consecutive_compacts = 0;
    std::string notice;  ///< 人类可读说明
};

/// @brief 排队消息条目（TUI 侧轻量拷贝，避免 action.h 依赖 agent 层 QueuedMessageItem）
struct QueueItemLite {
    std::string id;            ///< uuid（单条移除用）
    std::string text;          ///< 用户文本（队列卡片展示）
    int64_t queued_at_ms = 0;  ///< 入队时刻（毫秒时间戳）
};

/// @brief 消息队列更新（模型忙碌时前端入队的用户消息）
/// @details ChatSession → MessageQueueUpdatedEvent → 本动作；空 items = 已清空，
///          队列卡片消失。TUI 据此重绘输入框上方的可折叠队列条。
struct ActionQueueUpdate {
    std::string session_id;
    std::vector<QueueItemLite> items;
};

/// @brief 子 Agent 进度增量（AgentTool → 订阅者）
/// @details v1.3.0：补充结构化字段，供第二层卡片渲染复用主会话 UI（思考卡/工具卡）。
struct ActionSubAgentProgress {
    std::string task_id;
    int32_t step_number = 0;
    std::string step_type;  ///< "thought"/"action"/"observation"/"final"
    std::string content;
    // --- v1.3.0 结构化字段（与 ReActStep 对应）---
    std::string thought_text;  ///< thought/final 的 LLM 文本
    std::string tool_name;     ///< action 的工具名
    std::string tool_input;    ///< action 的工具参数 JSON 字符串
    std::string observation;   ///< observation 的工具结果文本
    bool is_error = false;     ///< 工具执行是否出错
    double duration_ms = 0.0;  ///< 本步骤耗时（毫秒，思考卡标签展示用）
};

/// @brief 子 Agent 完成（AgentTool → 订阅者）
struct ActionSubAgentCompleted {
    std::string task_id;
    std::string final_answer;
    bool was_error = false;
    double duration_ms = 0.0;
};

/// @brief 请求 UI 关闭（/exit）
struct ActionShutdown {};

/// @brief 临时提示（toast / 命令回显）
struct ActionToast {
    std::string text;
    bool is_error = false;
};

/// @brief 模型列表加载完成（App 后台线程 list_models 后入队）
struct ActionModelsLoaded {
    std::vector<std::string> models;
    std::vector<agent::ModelInfo>
        models_info;  ///< 完整模型信息（含 context_length，切换时解析窗口）
};

/// @brief 会话列表条目（UI 侧轻量拷贝，避免 action.h 依赖 SessionStore）
struct SessionLite {
    std::string title;         ///< 会话标题（无则回退 session_id）
    std::string file_path;     ///< JSONL 文件路径（恢复用）
    std::string project_name;  ///< 项目名称（cwd 末级目录名，展示用）
    int message_count = 0;
};

/// @brief 会话列表加载完成（App 后台线程 list_sessions 后入队）
struct ActionSessionsLoaded {
    std::vector<SessionLite> sessions;
};

/// @brief 供应商切换完成（后台 create_backend 成功后入队；provider 移交 UI 线程）
/// @details UI 线程处理时执行 set_provider + import_messages（保留对话继续），
///          成功后再写配置（apply_provider_switch）。
struct ActionProviderSwitched {
    std::unique_ptr<agent::ICompletionProvider> provider;  ///< 新后端（已 initialize）
    std::string model_name;                                ///< 新模型名（可能为空）
    agent::ProviderConfigEntry entry;                      ///< 已选条目（写配置用）
};

/// @brief 供应商切换失败（后台 create_backend 创建/初始化失败）
struct ActionProviderSwitchFailed {
    std::string provider_name;  ///< 显示名（提示用）
};

/// @brief 待办清单更新（#24：TodoStore → 侧边栏 TODO 区块 / StatusBar 进度）
/// @details 携带该 session 完整清单快照，ViewModel 更新 sidebar.todos 触发重绘。
struct ActionTodoUpdate {
    std::string session_id;
    std::vector<core::todo::TodoItem> todos;
};

/// @brief MCP server 状态条目（#27 M4：侧栏展示）
struct McpServerLite {
    std::string name;      ///< server 名
    std::string protocol;  ///< 协商协议版本（"2026-07-28" / "2025-11-25"）
    int tool_count = 0;    ///< 已预取工具数
    int state = 0;         ///< 0=连接中 1=已连接 2=失败
    std::string error;     ///< 失败原因（state==2 时）
};

/// @brief MCP server 状态更新（#27 M4：启动时查询 + 后台连接事件驱动）
/// @details ViewModel 更新 sidebar.mcp_servers 触发侧栏 MCP 区块重绘。
struct ActionMcpStatus {
    std::vector<McpServerLite> servers;
};

/// @brief 项目文件树节点（项目 tab）
/// @details 由后台 git 扫描线程构造并随 ActionProjectFiles 投递；目录用
///          children 嵌套，文件用 status 标记 git 状态点。rel_path 为相对项目根
///          的 '/' 分隔路径（目录/文件唯一键，合并保留展开状态用）。
struct ProjectNode {
    std::string name;      ///< 展示名（叶子名）
    std::string rel_path;  ///< 相对项目根路径（合并/展开键）
    bool is_dir = false;
    bool expanded = false;  ///< 目录展开状态（UI 线程维护；默认收起，避免自动全部展开）
    char status = ' ';        ///< git porcelain 状态码（'M'/'A'/'D'/'R'/'?'，' '=clean）
    bool has_status = false;  ///< 是否在 git 中处于非干净状态（渲染状态点）
    std::vector<ProjectNode> children;  ///< 子节点（仅目录）
};

/// @brief 项目文件树快照（后台 git 扫描完成 → UI 线程）
/// @details 携带完整树 + 项目根 + 是否 git 仓库；ViewModel 合并入 tabs.project
///          （保留既有目录展开状态）。
struct ActionProjectFiles {
    std::string root;               ///< 项目根目录（相对路径解析基准）
    bool is_git = false;            ///< 是否为 git 仓库
    bool loading = false;           ///< 仅用于初始化占位（true=加载中）
    std::vector<ProjectNode> tree;  ///< 根 children
};

/// @brief Hook 执行进度（订阅 HookProgressEvent → 输入区上方进度条）
/// @details hook_id 关联同一条 hook 的 start 与 done/failed 两拍，UI 据此
///          合并为一条卡片（进行中用 spinner，结束打勾/叉）。
struct ActionHookProgress {
    uint64_t hook_id = 0;
    std::string event;       ///< PreToolUse/PostToolUse/Stop/...
    std::string phase;       ///< start / done / failed
    std::string hook_type;   ///< command / http / prompt / agent
    std::string tool_name;   ///< 关联工具名
    std::string message;     ///< 执行结果摘要（done/failed）
    std::string hook_label;  ///< 展示标签
};

/// @brief 统一动作类型
using Action =
    std::variant<ActionAppendMessage, ActionAppendSkill, ActionAppendCmdResult,
                 ActionSubmitCmdToModel, ActionTokenDelta, ActionReasoningDelta, ActionStepDone,
                 ActionTurnDone, ActionError, ActionBeginTool, ActionEndTool, ActionAgentDone,
                 ActionSetBusy, ActionPermissions, ActionSetMode, ActionAskUser,
                 ActionAskUserTimeout, ActionOpenPlan, ActionCacheDiagnostics,
                 ActionCompactionPaused, ActionQueueUpdate, ActionSubAgentProgress,
                 ActionSubAgentCompleted, ActionHookProgress, ActionShutdown, ActionToast,
                 ActionModelsLoaded, ActionSessionsLoaded, ActionProviderSwitched,
                 ActionProviderSwitchFailed, ActionTodoUpdate, ActionMcpStatus, ActionProjectFiles>;

}  // namespace ftxtui