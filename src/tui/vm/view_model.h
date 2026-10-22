/**
 * @file view_model.h
 * @brief ViewModel — 文档模型（UI 线程独有），Apply 后台 action
 * @details 所有 UI 状态归 UI 线程读写；后台只通过 ActionQueue 推送 action。
 *          本文件只做数据变更，渲染在 app.cpp 单独进行。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bridge/action.h"
#include "core/todo/todo_item.h"  // #24：侧边栏 TODO 条目（状态图标渲染）
#include "core/utils/line_diff.h"
#include "vm/message_node.h"

namespace ftxtui {

/// @brief MCP server 侧栏条目（#27 M4：状态点 + 失败错误信息）
struct McpServerEntry {
    std::string name;      ///< server 名
    std::string protocol;  ///< 协商协议版本（"2026-07-28" / "2025-11-25"）
    int tool_count = 0;    ///< 已预取工具数
    int state = 0;         ///< 0=连接中 1=已连接 2=失败
    std::string error;     ///< 失败原因（state==2 时）
    bool operator==(const McpServerEntry&) const = default;
};

/// @brief 侧栏模型
struct SidebarModel {
    std::string title;       ///< 会话标题
    std::string model;       ///< 模型名
    std::string project;     ///< 项目名
    std::string branch;      ///< git 分支
    int context_limit = 0;   ///< 上下文上限（token）
    int context_used = 0;    ///< 已用（token）
    double cost_usd = 0.0;   ///< 会话成本
    std::string permission;  ///< 权限模式标签 "" / "bypass"（手动审批 / 完全访问）
    std::string mode;        ///< 工作模式标签 "standard" / "plan" / "minimal"
    bool visible = true;     ///< 侧栏是否可见（窄屏折叠）

    // 3.1 统计指标（来自 ActionTurnDone 用量，会话累计）
    int32_t cache_read_tokens = 0;  ///< DS/Anthropic 缓存读取（命中）token
    int32_t total_tokens = 0;       ///< 会话累计 token
    // #65：细粒度分项（会话累计）
    int32_t prompt_tokens = 0;      ///< 会话累计 prompt token
    int32_t generated_tokens = 0;   ///< 会话累计 generated token
    int32_t cache_hit_tokens = 0;   ///< DS 缓存命中 token（累计）
    int32_t cache_miss_tokens = 0;  ///< DS 缓存未命中 token（累计）

    // 3.2 MCP 列表（#27 M4：后台连接状态驱动，含状态点 + 失败错误信息）
    std::vector<McpServerEntry> mcp_servers;
    // 3.3 TODO 列表（#24：TodoStore 事件驱动，含状态供图标渲染）
    std::vector<core::todo::TodoItem> todos;
    // 可折叠区块展开状态（MCP / TODO 标题行点击切换）
    bool mcp_expanded = true;
    bool todo_expanded = true;
};

/// @brief 折叠卡片默认配置（新卡片创建时的展开/收起初始值）
struct CardDefaults {
    bool reasoning_expanded = true;  ///< 思考卡默认展开
    bool tool_expanded = false;      ///< 工具卡默认收起
};

/// @brief 侧边栏 tab 枚举（任务调度 | 项目 | 变更记录 | 文件）
enum class SidebarTab { kTasks = 0, kProjects, kFiles, kChanges, kCount };

/// @brief 子 Agent 聚合条目（任务调度 tab）
struct SubAgentLite {
    std::string task_id;
    std::string status;        ///< "running" / "done" / "failed"
    std::string current_step;  ///< 最近一步类型/摘要
    int step_number = 0;
    double duration_ms = 0.0;
    std::size_t msg_index = 0;  ///< 关联转录消息索引（跳转用）
};

/// @brief 子 Agent 单步记录（第二层独立渲染）
struct SubAgentStep {
    int step_number = 0;
    std::string step_type;  ///< "thought"/"action"/"observation"/"final"
    std::string content;    ///< 格式化行（保留，侧栏聚合/回退用）
    // --- v1.3.0 结构化字段（与主会话卡片渲染对齐）---
    std::string thought_text;  ///< thought/final 的 LLM 文本
    std::string tool_name;     ///< action 的工具名
    std::string tool_input;    ///< action 的工具参数 JSON 字符串
    std::string observation;   ///< observation 的工具结果文本
    bool is_error = false;     ///< 工具执行是否出错
    bool done = false;         ///< action 是否已关联 observation
    bool expanded = false;     ///< 卡片展开状态（思考卡/工具卡）
    double duration_ms = 0.0;  ///< 本步骤耗时（毫秒，思考卡标签展示用）
};

/// @brief 子 Agent 完整记录（第二层：独立渲染，不混入主转录区）
struct SubAgentDetail {
    std::string task_id;
    std::string status;  ///< "running" / "done" / "failed"
    int step_number = 0;
    std::vector<SubAgentStep> steps;
    std::string final_answer;
    double duration_ms = 0.0;
    bool reasoning_expanded = true;  ///< 思考卡展开状态（v1.3.0，点击切换）
};

/// @brief 输出区域层级（标题栏下子列表导航）
enum class OutputLevel { Main = 0, SubAgent = 1 };

/// @brief 后台任务条目（任务调度 tab）
struct TaskLite {
    std::string name;
    std::string status;  ///< Pending/Running/Completed/Cancelled/Failed
    float progress = 0.0f;
};

/// @brief 会话内一次文件修改（Edit/Write 工具调用，内联 diff 高亮数据源）
struct FileChange {
    std::string file_path;
    std::string purpose;     ///< 修改目的（该步 reasoning 摘要，单行）
    std::string reasoning;   ///< 完整 reasoning（变更记录 tab 按 e 展开）
    std::string old_string;  ///< Edit 旧内容（Write 全量改写时为空）
    std::string new_string;  ///< 新内容
    int64_t timestamp = 0;
    std::size_t msg_index = 0;          ///< 关联转录消息索引（跳转用）
    std::vector<agent::DiffLine> diff;  ///< 行级 diff（仅 Equal/Insert/Modify）
    int new_start = 0;  ///< 修改区块在文件中的起始行（/view 打开时定位，1-based）
};

/// @brief 解码后的图片数据（RGBA；加载时已下采样控制内存）
struct ImageData {
    int width = 0;              ///< 像素宽
    int height = 0;             ///< 像素高
    std::vector<uint8_t> rgba;  ///< RGBA 像素（width*height*4）
};

/// @brief 文件 tab 状态（/view 只读查看器）
struct FileViewState {
    std::string path;                  ///< 当前查看文件（空=未打开）
    std::vector<std::string> lines;    ///< 当前内容（按行；图片视图为空）
    std::string lang;                  ///< 高亮语言（扩展名推断）
    int scroll = 0;                    ///< 首行索引（虚拟化滚动）
    bool dirty = false;                ///< /edit 后需重读（P6 联动）
    std::vector<FileChange> changes;   ///< 该文件会话内修改（内联高亮用）
    std::shared_ptr<ImageData> image;  ///< 图片视图数据（非空 = 图片预览模式）
};

/// @brief 变更记录 tab 状态
struct ChangeViewState {
    std::vector<FileChange> changes;  ///< 会话内全部修改（按文件分组）
    int selected = 0;                 ///< 选中修改点（-1=无）
    bool purpose_expanded = false;    ///< e 展开完整 reasoning
};

/// @brief 项目文件树状态（项目 tab，常驻）
struct ProjectTreeState {
    bool loading = true;  ///< 后台 git 扫描进行中（未完成时显示加载占位）
    bool ready = false;   ///< 首轮扫描已完成
    bool is_git = false;  ///< 项目根是否为 git 仓库
    std::string root;     ///< 项目根目录（相对路径解析基准）
    int scroll = 0;       ///< 扁平可视行滚动偏移（虚拟化滚动）
    std::vector<ProjectNode> tree;  ///< 根 children（ProjectNode 定义于 bridge/action.h）
};

/// @brief 侧边栏 tab 模型（任务调度 | 项目 | 变更记录 | 文件）
struct SidebarTabsModel {
    SidebarTab active = SidebarTab::kTasks;
    bool changes_open = false;  ///< 变更记录 tab 是否打开（有 FileChange 时自动开）
    bool file_open = false;     ///< 文件 tab 是否打开（/view 打开）
    // —— 任务调度 ——
    bool busy = false;
    std::string current_tool;  ///< 当前执行中的工具名
    int step_number = 0;
    int total_steps = 0;
    std::vector<SubAgentLite> sub_agents;
    std::vector<TaskLite> background_tasks;
    int sub_selected = -1;  ///< 选中子 Agent 索引（-1=无；方向键/Enter 交互）
    // —— 项目文件树（常驻 tab，后台 git 扫描驱动）——
    ProjectTreeState project;
    // —— 文件 / 变更记录 ——
    FileViewState file;       ///< 文件 tab 状态（/view 只读查看器）
    ChangeViewState changes;  ///< 变更记录 tab 状态（会话内全部修改）
};

/// @brief 消息队列状态（模型忙碌时前端入队的用户消息；输入框上方队列卡片）
struct MessageQueueState {
    std::vector<QueueItemLite> items;  ///< 排队消息（空 = 无排队，卡片隐藏）
    bool expanded = false;             ///< 队列条是否展开（展开显示逐条预览）
};

/// @brief 顶层视图模型
class ViewModel {
   public:
    std::vector<MessageNode> messages;
    SidebarModel sidebar;
    SidebarTabsModel tabs;       ///< 侧边栏 tab 状态（任务调度 | 变更记录 | 文件）
    bool busy = false;           ///< 是否正在生成/忙碌
    std::string prompt_echo;     ///< 待显示的命令回显/提示
    bool pending_exit = false;   ///< 收到 /exit，UI 应退出
    CardDefaults card_defaults;  ///< 折叠卡片默认配置
    MessageQueueState message_queue;  ///< 消息队列（模型忙碌时缓存用户输入）

    // ---- 输出区域层级（标题栏下子列表导航）----
    OutputLevel output_level = OutputLevel::Main;  ///< 当前输出层级（主会话 / 子 Agent）
    int sub_active = -1;  ///< 当前查看的子 Agent 记录索引（-1 = 无）
    std::vector<SubAgentDetail> sub_records;  ///< 子 Agent 完整记录（第二层独立渲染）

    // ---- Hook 执行进度（#50：输入区上方进度条）----
    /// @brief 展示面板内的一条 hook 进度（hook_id 关联 start/done 两拍）
    struct HookRow {
        uint64_t hook_id = 0;
        std::string event;      ///< PreToolUse/Stop/...
        std::string hook_type;  ///< command/http/prompt/agent
        std::string tool_name;  ///< 关联工具名
        std::string label;      ///< 展示标签
        std::string phase;      ///< start/done/failed
        std::string message;    ///< 结果摘要
    };
    static constexpr std::size_t kMaxHookRows = 8;  ///< 面板最多保留的行数（FIFO 淘汰）
    std::vector<HookRow> hook_progress;

    /// @brief 应用一个动作
    /// @return 状态是否有变化（用于决定是否重绘）
    bool apply(const Action& action);

    /// @brief 获取当前正在流式的消息（不存在则创建一条 assistant）
    MessageNode& active_stream();
    bool has_active_stream() const;

    /// @brief 会话内累计 token（本实现用最后一次 TurnDone 的统计近似）
    int32_t total_tokens = 0;

   private:
    // 单 action 分派（由 apply 的 std::visit 调用）
    bool apply_variant(const ActionAppendMessage&);
    bool apply_variant(const ActionAppendSkill&);
    bool apply_variant(const ActionAppendCmdResult&);
    bool apply_variant(const ActionSubmitCmdToModel&);
    bool apply_variant(const ActionTokenDelta&);
    bool apply_variant(const ActionReasoningDelta&);
    bool apply_variant(const ActionStepDone&);
    bool apply_variant(const ActionTurnDone&);
    bool apply_variant(const ActionError&);
    bool apply_variant(const ActionBeginTool&);
    bool apply_variant(const ActionEndTool&);
    bool apply_variant(const ActionAgentDone&);
    bool apply_variant(const ActionSetBusy&);
    bool apply_variant(const ActionPermissions&);
    bool apply_variant(const ActionSetMode&);
    bool apply_variant(const ActionAskUser&);
    bool apply_variant(const ActionAskUserTimeout&);
    bool apply_variant(const ActionOpenPlan&);
    bool apply_variant(const ActionCacheDiagnostics&);
    bool apply_variant(const ActionCompactionPaused&);
    bool apply_variant(const ActionQueueUpdate&);
    bool apply_variant(const ActionSubAgentProgress&);
    bool apply_variant(const ActionSubAgentCompleted&);
    bool apply_variant(const ActionHookProgress&);
    bool apply_variant(const ActionShutdown&);
    bool apply_variant(const ActionToast&);
    bool apply_variant(const ActionModelsLoaded&);
    bool apply_variant(const ActionSessionsLoaded&);
    bool apply_variant(const ActionProviderSwitched&);
    bool apply_variant(const ActionProviderSwitchFailed&);
    bool apply_variant(const ActionTodoUpdate&);
    bool apply_variant(const ActionMcpStatus&);
    bool apply_variant(const ActionProjectFiles&);

    /// @brief 修改追踪：Edit/Write 工具调用 → FileChange（purpose + 行级 diff）
    void track_file_change(const ActionBeginTool& a);
};

}  // namespace ftxtui