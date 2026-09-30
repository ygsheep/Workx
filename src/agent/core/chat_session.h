/**
 * @file chat_session.h
 * @brief 对话状态机
 * @details 持有 ICompletionProvider，处理用户事件，后台推理，发布流式事件
 * @version 3.1.0
 * @date 2026-07
 */

#pragma once

#include <format>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <functional>
#include <unordered_set>
#include <utility>  // C-1：std::pair
#include "agent/api/i_completion_provider.h"
#include "agent/api/chat_types.h"
#include "agent/api/retry.h"  // H-3：HttpRetryPolicy
#include "agent/core/react_observer.h"
#include "core/events/event_bus.h"
#include "core/events/agent_events.h"  // QueuedMessageItem / MessageQueueUpdatedEvent
#include "core/config/config_manager.h"
#include "core/utils/result.h"
#include "agent/message/types.h"
#include "agent/tool/registry.h"
#include "agent/tool/executor.h"
#include "agent/tool/context.h"  // #45：PermissionMode / SessionMode（会话级权限 + 工作模式）
#include "agent/compact/prefix_shape.h"           // DS_CACHE: 前缀形状追踪
#include "agent/compact/cache_aware_compactor.h"  // DS_CACHE H-3: 跨 turn 持久化的压缩器
#include "agent/session/session_store.h"          // 项目会话恢复：JSONL 持久化
#include "agent/skill/inclaude/conditional.h"  // conditional skills：TouchCollector（按值成员）
#include "core/task/task_manager.h"

// #54：Plan Mode V2 会话语义级协调器（五阶段多 Agent 规划）
// 注意：实现位于 agent::plan，前向声明必须进同名命名空间，
//       否则 ChatSession 成员 unique_ptr<plan::PlanCoordinator> 会误绑定到 ::plan 占位。
namespace agent::plan {
class PlanCoordinator;
}

namespace agent {

// 前向声明（conditional skills 支持）
namespace command {
class CommandRegistry;
}

// #56 方案 D：MCP 连接管理器前向声明（shared_ptr 成员）
namespace mcp {
class McpClientManager;
}

// #50：会话级 HookManager 前向声明（shared_ptr 成员）
namespace hook {
class HookManager;
}

/// @brief 后端接口前向声明（供 ChatSession::backend() 返回类型使用）
class IBackend;

/// @brief H-7：重试决策动作
/// @details compute_retry() 纯函数的返回类型，描述下一步动作
enum class RetryAction {
    Continue,  ///< 不重试，继续执行（无错误或不可重试错误）
    Stop,      ///< 不可重试错误，发布终止事件
    Sleep      ///< 可重试，sleep delay_ms 后递归调用 run_completion
};

/// @brief H-7：重试决策
/// @details 纯函数 compute_retry() 的返回值，分离纯逻辑与 I/O 副作用
struct RetryDecision {
    RetryAction action = RetryAction::Stop;  ///< 决策动作
    int delay_ms = 0;                        ///< Sleep 时的退避时长（毫秒）
};

/// @brief 对话会话
/// @details 由外部驱动（main.cpp），通过 send_message() 提交文本，
///          后台 Task 调用 ICompletionProvider，发布
///          StreamTokenEvent/StepDoneEvent/StreamDoneEvent。 支持工具调用（function calling）：LLM
///          返回 tool_use → 执行工具 → tool_result → 继续推理
class WORKX_API ChatSession {
   public:
    /// @brief ReAct 循环事件发布器（3.2 IReActObserver 实现）
    /// @details 将 ReActLoop 步骤事件转换为 IEventBus 异步事件，
    ///          使 ReActLoop 可脱离 EventBus 体系独立使用与测试。
    class ReActEventPublisher : public IReActObserver {
       public:
        explicit ReActEventPublisher(IEventBus& bus, std::string session_id);
        ~ReActEventPublisher() override = default;

        void on_thought(const ReActStep& step) override;
        void on_action(const ReActStep& step) override;
        void on_observation(const ReActStep& step) override;
        void on_final_answer(const ReActStep& step) override;
        void on_token(const std::string& content_delta,
                      const std::string& reasoning_delta) override;

       private:
        IEventBus& m_bus;
        std::string m_session_id;
    };
    /// @brief 构造
    /// @param provider 推理提供者（IBackend 或 IAgentCore）
    /// @param task_manager 任务管理器（H-4：DI 必须显式注入，无默认实参回退单例）
    /// @param event_bus 事件总线（H-4：DI 必须显式注入，无默认实参回退单例）
    /// @param config_manager 配置管理器（H-4：DI 必须显式注入，无默认实参回退单例）
    /// @param retry_delay_ms 重试初始延迟（毫秒），会被 backend.retry_delay_ms 覆盖
    /// @param session_id 会话标识（用于事件流区分多会话，默认 "default"）
    explicit ChatSession(std::unique_ptr<ICompletionProvider> provider, ITaskManager& task_manager,
                         IEventBus& event_bus, IConfigManager& config_manager,
                         int retry_delay_ms = 1000, std::string session_id = "default");

    ~ChatSession();

    /// @brief 添加系统提示词
    void set_system_prompt(const std::string& prompt);

    /// @brief 设置系统提示词重建回调（方案 A）
    /// @details 会话在切换工作模式（极简/标准/计划）时调用该回调，按目标模式重建
    ///          系统提示词（极简模式只拼白名单工具说明），使提示词的工具介绍段
    ///          与 function-calling schema 过滤保持联动。nullptr 表示不重建。
    void set_system_prompt_builder(std::function<std::string(tool::SessionMode)> builder);

    /// @brief 获取系统提示词（返回拷贝，线程安全）
    std::string system_prompt() const;

    /// @brief 设置工具注册表（启用 function calling）
    /// @param registry 工具注册表（含已注册的工具实例）
    void set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry);

    /// @brief 获取工具注册表（返回拷贝，线程安全）
    std::shared_ptr<tool::ToolRegistry> tool_registry() const;

    /// @brief 设置宿主文件索引失效回调（可选）
    /// @details FileWriteTool 写入文件后调用（如 TUI @ 补全索引 mark_dirty）。
    ///          必须在首次 send_message 前调用；不设置则工具写文件不通知宿主。
    /// @param invalidator 宿主回调（空 = 取消注册）
    void set_file_index_invalidator(std::function<void()> invalidator);

    /// @brief 设置命令注册表（conditional skills 激活匹配用）
    /// @param registry 命令注册表（含磁盘 skill 命令）
    void set_command_registry(std::shared_ptr<command::CommandRegistry> registry);

    /// @brief 获取命令注册表（返回拷贝，线程安全）
    std::shared_ptr<command::CommandRegistry> command_registry() const;

    /// @brief 设置 MCP 连接管理器（#56 方案 D）
    /// @param manager 父会话全局 MCP 管理器（AgentTool 子 Agent mcpServers 引用复用来源）
    void set_mcp_manager(std::shared_ptr<mcp::McpClientManager> manager);

    /// @brief 获取 MCP 连接管理器（返回拷贝，线程安全）
    std::shared_ptr<mcp::McpClientManager> mcp_manager() const;

    /// @brief 获取 touch 收集器（conditional skills 用）
    /// @return TouchCollector 引用（会话内有效）
    skill::TouchCollector& touch_collector();

    /// @brief 清空对话历史
    void clear_history();

    /// @brief 重新生成最后一条回复
    void regenerate();

    /// @brief 从指定用户消息重新生成（重试按钮：截断到该用户消息后重新推理）
    /// @param user_text 触发该回复的用户消息文本（从后往前匹配最后一条相同文本）
    /// @details 与 regenerate() 的区别：regenerate() 只重生成最后一条回复；
    ///          本方法先截断到匹配的用户消息（含）之后的所有消息，再以该用户
    ///          消息重新生成，支持对历史任意一条回复做重试。
    void regenerate_from(const std::string& user_text);

    /// @brief 获取对话历史（返回拷贝，线程安全）
    std::vector<ChatMessage> get_messages() const;

    /// @brief DS_CACHE H-4：配置压缩器上下文窗口（从 provider preset 注入）
    /// @details 必须在首次 send_message 前调用。影响压缩水位判定。
    void set_compactor_context_window(int32_t context_window_tokens);

    /// @brief DS_CACHE M-1：配置压缩器归档目录（compact 折叠前归档原消息）
    /// @details 必须在首次 maybe_compact 前调用。非空时折叠的中段消息会被
    ///          序列化到 <archive_dir>/<timestamp>.jsonl，保证可追溯。
    void set_compactor_archive_dir(const std::string& dir);

    /// @brief 手动压缩上下文（搜索面板「压缩上下文」/ /compact 命令调用）
    /// @details 在 m_state_mutex 保护下就地压缩 m_messages（与 ReActLoop 构建请求
    ///          前自动压缩同一入口）；返回压缩结果。调用方须先确认
    ///          is_generating()==false，避免与后台任务竞争 m_messages。
    CacheAwareCompactor::Result compact_context();

    /// @brief 设置 SessionStore（可选，设置后每条消息实时持久化到 JSONL）
    /// @details 必须在首次 send_message 前调用。设置后：
    ///          - user 消息 push_back 后立即 append 到 JSONL
    ///          - run() 返回后批量 append 新增的 assistant/tool 消息
    ///          - restore_from_file 加载的历史不会重复持久化
    void set_session_store(std::shared_ptr<agent::session::SessionStore> store);

    /// @brief 追加手动调用技能事件（合成 Skill 卡持久化，/resume 重建转录显示）
    /// @details 需在首条消息后、store 可用时调用；无 store 时静默忽略。
    ///          query 字段用于 /resume 时定位对应会话 user 消息，转为其"原始输入 + Skill 卡"。
    void append_skill_event(const agent::session::SkillEvent& ev);

    /// @brief 配置懒创建 SessionStore 的参数（首条 user 消息时才创建文件）
    /// @details factory 调用此方法传入配置，不立即创建文件。
    ///          首条 user 消息持久化时才创建 SessionStore + 写 session_start + title。
    /// @param project_dir 项目会话目录（<config_dir>/projects/<编码路径>）
    /// @param cwd 当前工作目录
    /// @param model 模型名
    /// @param git_branch git 分支
    void configure_session_store(const std::string& project_dir, const std::string& cwd,
                                 const std::string& model, const std::string& git_branch);

    /// @brief 从 JSONL 文件加载历史会话消息
    /// @param file_path JSONL 文件路径
    /// @return true=加载成功（至少有一条消息）
    /// @details 加载后消息追加到 m_messages（不清空已有消息）。
    ///          加载的消息不会触发持久化（避免回环）。
    bool restore_from_file(const std::string& file_path);

    /// @brief 新建会话（/new 与 /clear 共用）：清空消息并切换到新 session_id
    /// @details 保留旧会话文件（/new 语义）；/clear 由调用方在切换后删除旧文件。
    ///          生成中安全（先取消并等待当前任务）：
    ///          1. 生成新 session_id（core::util::generate_uuid）
    ///          2. 清空 m_messages
    ///          3. 关闭旧 SessionStore 并置空（新会话文件懒创建：首条 user 消息时
    ///             以新 session_id 创建 JSONL，复用启动时 configure_session_store 参数）
    ///          4. 重置压缩器与前缀形状基线 + conditional skills 会话级累积
    ///          锁外重置 TodoStore 到新会话空清单（restore_todos 内部再加 m_state_mutex）。
    void new_session();

    /// @brief 切换到历史会话（/resume 命令调用）
    /// @param file_path 历史会话 JSONL 文件路径
    /// @return true=切换成功
    /// @details 原子操作（单一锁作用域）：
    ///          1. load_messages + load_meta 加载历史
    ///          2. 替换 m_session_id（从文件名 stem 提取）
    ///          3. 清空 m_messages，填入历史消息
    ///          4. 关闭旧 SessionStore，创建新 SessionStore 指向历史文件，append 模式打开
    ///          5. 不追加 session_start（会话进行中，只是换文件继续写）
    ///          6. 重置压缩器和前缀形状基线
    ///          不写 session_end 到旧文件（会话可被多次 resume 继续）。
    bool switch_session(const std::string& file_path);

    /// @brief 从内存导入消息历史（/provider 热切换后保留当前对话继续）
    /// @param messages 待导入的消息（通常来自旧 session 的 get_messages）
    /// @details 清空现有消息后填入，并重置压缩器与前缀形状基线（与 switch_session
    ///          保持一致），不涉及文件 I/O、不改变 SessionStore。
    void import_messages(std::vector<ChatMessage> messages);

    /// @brief 修改当前会话标题（/rename 命令调用）
    /// @param title 新标题
    /// @return true=成功追加 title 事件
    /// @details append-only：追加新 title 事件到当前 JSONL，读取时取最后一条。
    bool rename_session(const std::string& title);

    /// @brief 获取 SessionStore（用于退出时写入 session_end）
    std::shared_ptr<agent::session::SessionStore> session_store() const { return m_session_store; }

    /// @brief 获取会话 ID
    const std::string& session_id() const { return m_session_id; }

    /// @brief C-2：暴露 completion provider 原始指针（用于 factory 获取 IBackendAdmin）
    /// @details 调用方（factory）可通过 dynamic_cast<IBackendAdmin*> 安全转型。
    ///          返回的指针生命周期由 ChatSession 管理，session 析构后禁止使用。
    ICompletionProvider* completion_provider() const { return m_provider.get(); }

    /// @brief 运行时切换推理后端（/provider 热切换）
    /// @param provider 新后端（须已 initialize；空指针忽略）
    /// @return true=已替换；false=生成中拒绝切换（调用方应先用 is_generating 检查）
    /// @details 线程安全（受 m_state_mutex 保护）。ReActLoop 每次 run 时从
    ///          m_provider 取指针（chat_session.cpp run_completion 内新建），
    ///          非生成中替换无并发访问。切换后调用方通常需 import_messages
    ///          保留对话继续（消息在 ChatSession 内，不受 provider 影响）。
    bool set_provider(std::unique_ptr<ICompletionProvider> provider);

    /// @brief #45：设置会话级权限模式（CLI --bypass-permissions 注入）
    /// @details 仅接受 Default/BypassPermissions（Plan 由 toggle_permission_mode 管理）。
    ///          线程安全（受 m_state_mutex 保护）。跨 turn 生效（下一轮 ReActLoop 注入）。
    void set_permission_mode(tool::PermissionMode mode);

    /// @brief #54：Plan Mode V2 协调器访问器（供宿主/工具读取阶段与规划产物）
    /// @return 协调器指针（为空表示未创建）；生命周期与 ChatSession 绑定
    plan::PlanCoordinator* plan_coordinator() const { return m_plan_coordinator.get(); }

    /// @brief #45：获取当前权限模式（线程安全）
    tool::PermissionMode permission_mode() const;

    /// @brief #45：两态切换权限模式（Default ↔ BypassPermissions）
    /// @details Shift+Tab 触发。计划模式已从权限循环独立为工作模式（toggle_session_mode
    ///          统一管理，进入 Plan 联动权限并记录 before_plan，退出时恢复）。
    ///          计划模式下直接忽略（避免与模式状态机打架）。线程安全。
    void toggle_permission_mode();

    /// @brief 获取当前会话工作模式（标准 / 计划 / 极简）
    /// @details 线程安全（受 m_state_mutex 保护）。模式与权限正交：
    ///          模式为顶层选择，权限（手动审批 / 完全访问）在模式内部切换。
    tool::SessionMode session_mode() const;

    /// @brief 设置会话工作模式
    /// @details 跨 turn 生效（下一轮 ReActLoop 注入；极简模式同时过滤工具 schema）。
    ///          进入 Plan 时联动权限=Plan（保存 before_plan），退出 Plan 时恢复原权限。
    ///          线程安全（受 m_state_mutex 保护）。
    void set_session_mode(tool::SessionMode mode);

    /// @brief 三态循环切换工作模式（Standard → Minimal → Plan → Standard）
    /// @details 模式切换键触发（TUI）。进入 Plan 记录 before_plan；离开 Plan 恢复。
    ///          线程安全（受 m_state_mutex 保护）。
    void toggle_session_mode();

    /// @brief #87：resume 时权限模式的恢复结果
    /// @details 由 switch_session 单点产出，TUI 只负责呈现——fail-safe 的三分支判定
    ///          属于 agent 层语义，若 TUI 再判一遍必然出现两处逻辑漂移。
    struct PermissionRestoreResult {
        bool restored = false;  ///< 是否从会话记录成功恢复
        /// @brief 未恢复的原因：""（已恢复）/ "no_record"（无 permission 事件）/
        ///        "invalid_value"（事件值非法或映射失败）
        std::string reason;
        tool::PermissionMode mode{tool::PermissionMode::Default};  ///< 恢复/回退后的实际模式
    };

    /// @brief #87：获取最近一次 switch_session 的权限恢复结果（线程安全）
    /// @details 供 TUI 在 resume 后决定是否提示用户。无人调用过 switch_session 时
    ///          返回 restored=false 且 reason 为空（表示「无需提示」）。
    PermissionRestoreResult last_permission_restore() const;

    /// @brief 是否正在生成
    bool is_generating() const { return m_generating.load(); }

    /// @brief 暂停模型活动：取消并等待当前后台任务完全退出
    /// @details 供宿主（TUI /edit 等）在需要独占文件/状态时调用，避免后台
    ///          ReActLoop 写文件与宿主操作冲突。线程安全（内部持锁）。
    void cancel_current_task() { cancel_and_wait_current_task(); }

    /// @brief 提交用户消息，触发 LLM 推理
    /// @param text 用户文本
    /// @param images 图片附件绝对路径（多模态，可为空）
    /// @details 模型忙碌（is_generating）时自动入队（不直接发送），空闲时正常触发推理。
    void send_message(const std::string& text, const std::vector<std::string>& images = {});

    // ============================================================
    // 消息队列（模型忙碌时缓存用户输入）
    // ============================================================

    /// @brief 入队一条用户消息（模型忙碌时缓存，空闲时直接发送不走队列）
    /// @details 线程安全。入队后发布 MessageQueueUpdatedEvent（TUI 队列卡片刷新）。
    ///          不影响 m_messages（未发送前不持久化）。返回是否实际入队
    ///          （模型空闲时入队无意义，调用方应直接 send_message）。
    /// @return true=已入队（模型忙碌）；false=未入队（模型空闲，应走 send_message）
    bool enqueue_message(const std::string& text, const std::vector<std::string>& images = {});

    /// @brief 请求立即冲刷队列（Ctrl+Enter）
    /// @details 置位 m_flush_requested，ReActLoop 在下一个工具轮边界调用
    ///          inject_pending_queue 把队列合并为单条 user 消息注入当前循环。
    void request_flush();

    /// @brief 单条移除队列消息（TUI 队列卡片 ✕ 按钮）
    /// @param id QueuedMessageItem.id
    void remove_queued_message(const std::string& id);

    /// @brief 清空队列（clear_history / new_session 时调用）
    void clear_pending_queue();

    /// @brief 获取当前队列快照（返回拷贝，线程安全）
    std::vector<QueuedMessageItem> queued_messages() const;

    /// @brief 合并排队消息为单条 user 消息文本（序号 + 分隔线）
    /// @details 公开为 public 以便单元测试直接验证合并格式（纯静态函数，无成员依赖）。
    /// @param items 排队消息（非空）
    static std::string merge_queued_text(const std::vector<QueuedMessageItem>& items);

    /// @brief 保存对话历史到文件
    /// @details H-6：仅做 serialize_state() → ofstream，序列化逻辑在 serialize_state() 中
    Result<void, std::string> save_session(const std::string& path) const;

    /// @brief 从文件加载对话历史
    /// @details H-6：仅做 ifstream → deserialize_state()，反序列化逻辑在 deserialize_state() 中
    Result<void, std::string> load_session(const std::string& path);

    /// @brief H-6：序列化会话状态为 JSON（纯函数，不触碰文件系统）
    /// @details 拷贝 system_prompt 与 messages 后构建 JSON，调用方无需加锁。
    ///          公开为 public 以便单元测试直接验证序列化格式。
    /// @return 包含 system_prompt（可选）和 messages 数组的 JSON 对象
    nlohmann::json serialize_state() const;

    /// @brief H-6：从 JSON 反序列化会话状态（C-1：真正纯函数，不触碰成员状态）
    /// @details 解析 system_prompt 与 messages，校验字段完整性。
    ///          仅对输入 j 进行只读解析，返回新构造的 (messages, system_prompt)，
    ///          调用方负责通过 commit_state() 提交到成员。
    ///          公开为 public 以便单元测试直接验证反序列化逻辑。
    /// @param j JSON 对象（来自 load_session 读取的文件）
    /// @return 成功返回 (messages, system_prompt)；失败返回错误信息
    static Result<std::pair<std::vector<ChatMessage>, std::string>, std::string> deserialize_state(
        const nlohmann::json& j);

    /// @brief C-1：提交反序列化结果到成员状态（加锁一次性写入）
    /// @details 将 deserialize_state 返回的 (messages, system_prompt) 原子提交。
    ///          load_session 内部调用；测试中也可直接构造 pair 后调用此方法
    ///          设置初始状态，避免依赖 deserialize_state 形成闭环测试。
    void commit_state(std::vector<ChatMessage> messages, std::string system_prompt);

    /// @brief H-7：纯函数计算重试决策
    /// @details 分离 5 类关注点中的纯逻辑部分：
    ///          ①可重试判定 ②退避延迟计算。
    ///          run_completion 仅按决策执行 I/O（事件发布/sleep/递归调用）。
    ///          公开为 public 以便单元测试直接验证决策。
    /// @param react_result ReAct 循环结果（只读 was_error / error_message）
    /// @param retry_policy HTTP 重试策略（提供 max_retries / is_retryable / delay_ms）
    /// @param attempt 当前重试次数（0=首次请求失败后的第一次重试判定）
    /// @return RetryDecision：Continue（无错误）/ Sleep（可重试）/ Stop（不可重试或超上限）
    /// C-3：移除 noexcept —— delay_ms 不再 noexcept，且调用链含 std::format 可能抛
    static RetryDecision compute_retry(const ReActResult& react_result,
                                       const HttpRetryPolicy& retry_policy, int attempt);

    // H-8：移除 backend() 方法。
    // 原设计暴露完整 IBackend* 给 UI 层，违反接口隔离（UI 可误调 shutdown() 等
    // IBackendAdmin 方法）。UI 层应通过 factory 独立注入的 IBackendAdmin* 调用
    // list_models / set_model_name 等管理方法。

   private:
    /// @brief 执行推理（在后台线程中运行，含 agent 循环）
    /// @param user_text 用户输入文本
    /// @param images 图片附件绝对路径（仅首次请求使用，重试沿用已入列的图片消息）
    /// @param retry_attempt 当前重试次数（0=首次请求）
    void run_completion(const std::string& user_text, const std::vector<std::string>& images = {},
                        int retry_attempt = 0);

    /// @brief 订阅中断事件
    void subscribe_interrupt();

    /// @brief 取消中断订阅
    void unsubscribe_interrupt();

    /// @brief 发布当前队列快照事件（TUI 队列卡片刷新；锁外调用，内部持 m_queue_mutex）
    void publish_queue_update();

    /// @brief 工具轮边界冲刷：请求冲刷且队列非空时，合并为单条 user 消息注入 messages
    /// @param messages 当前循环的消息列表（ReActLoop 线程直接写入，无额外锁）
    /// @details 由 ReActLoop 通过 GoalAgentDeps.queue_injector 回调调用。
    void inject_pending_queue(std::vector<ChatMessage>& messages);

    /// @brief 整轮收尾冲刷：run_completion 结束时若队列仍有未发送消息，自动开启新一轮
    /// @details 合并队列为单条 user 消息并调用 run_completion 继续发送；
    ///          仅在 m_generating 已复位为 false 后调用（防止与当前任务并发）。
    void flush_pending_after_run();

    /// @brief 订阅子 Agent 进度/完成事件并持久化到 SessionStore
    /// @details 第二层（子 Agent 记录）持久化：SubAgentProgressEvent/SubAgentCompletedEvent
    ///          发布时追加 sub_agent 事件到当前 SessionStore，/resume 时按序重放恢复。
    void subscribe_sub_agent_persistence();

    /// @brief 取消子 Agent 事件持久化订阅
    void unsubscribe_sub_agent_persistence();

    /// @brief #54：订阅 Plan Mode V2 事件（Enter/Exit/子 Agent 完成 → 驱动协调器流转）
    /// @details 进入 Plan 触发 interview→explore；子 Agent 完成回报 explore 发现；
    ///          Exit 批准/驳回推进阶段终态。产物就绪（AwaitingApproval）时落盘方案文件。
    void subscribe_plan_events();

    /// @brief #54：取消 Plan Mode V2 事件订阅
    void unsubscribe_plan_events();

    /// @brief #54：计划产物就绪（AwaitingApproval）时落盘方案 markdown（供退出确认预览）
    void maybe_persist_plan_artifact();

    /// @brief DS_CACHE M-4：LLM 摘要回调（注入到 m_compactor）
    /// @param middle 待摘要的中段消息序列
    /// @return LLM 生成的摘要文本（失败时抛异常，由 compact_middle fallback 到机械折叠）
    /// @details 同步调用 m_provider 完成摘要生成：
    ///          1. 构造摘要 system prompt + middle 消息
    ///          2. submit_completion 提交
    ///          3. 阻塞 next() 读取流直到 Complete，拼接 content
    ///          4. 失败（nullptr/Error/Cancelled）抛异常触发 fallback
    std::string summarize_with_llm(const std::vector<ChatMessage>& middle);

    /// @brief 持久化单条消息到 SessionStore（如果已设置）
    /// @details 根据 msg.role 调用对应的 append 方法。
    ///          uuid 由 core::util::generate_uuid() 生成，timestamp 由 now_iso() 生成。
    void persist_message(const ChatMessage& msg);

    /// @brief 批量持久化 [start_idx, end) 范围的消息
    /// @param start_idx 起始索引（含）
    /// @param parent_uuid 父消息 UUID（用于 parentUuid 字段）
    void persist_messages_range(size_t start_idx, const std::string& parent_uuid = "");

    /// @brief 生成中安全：取消并等待当前后台任务完全退出后，再复位生成标志
    /// @details 切换/清理会话共享状态（m_messages / m_compactor / m_session_store）前必须先调用。
    ///          因为 ReActLoop::run 通过非 const 引用直接读写 m_messages，若调用方（UI 线程）
    ///          并发执行 switch_session/clear_history/import_messages 对 m_messages 做 move/clear，
    ///          会与任务线程形成数据竞争 → 堆损坏（resume 后重发消息崩溃的 UAF 根因）。
    ///          复刻析构 wait 模式：cancel 当前任务 + wait → cancelAll + waitForAll → 复位标志。
    void cancel_and_wait_current_task();
    void cancel_background_tasks();  // 定向取消本会话已分发的后台任务（P1-1）

    /// @brief #24：接线 TodoStore 持久化回调（session_id → 当前 SessionStore 写 todo 事件）
    /// @details 在 SessionStore 创建/切换后调用（懒创建 + switch_session）。
    ///          回调捕获 store 的 shared_ptr，TodoStore 每次变更时追加全量快照。
    void wire_todo_persistence();

    /// @brief 记录 system_prompt 事件到 SessionStore（会话轨迹调试）
    /// @param reason 记录原因：initial / changed / resume
    /// @details store 未创建时置 m_pending_system_prompt，待懒创建后补写 initial。
    ///          仅在提示词内容相对上次记录有变化时落盘，避免重复快照。
    void persist_system_prompt(const std::string& reason);

    /// @brief 按当前会话模式重建系统提示词（方案 A）
    /// @details 调用方必须已持有 m_state_mutex。回调非空且重建结果与现提示词不同时
    ///          覆写 m_system_prompt 并返回落盘 reason（changed/initial），无变化返回空串。
    /// @return 非空表示需要锁外调用 persist_system_prompt 落盘
    std::string rebuild_system_prompt_locked();

    /// @brief #87：把当前权限/工作模式三态追加为 permission 事件落盘
    /// @details 必须在**持有 m_state_mutex** 时调用（名字后缀 _locked 即此约定）。
    ///          无 SessionStore（未开会话 / 内存会话）时静默跳过——持久化是能力增强，
    ///          不是主流程，不能因写盘失败影响模式切换本身。
    void persist_permission_state_locked();

    /// @brief #87：从 permission 事件恢复三态，并产出恢复结果（供 TUI 提示）
    /// @details 必须在**持有 m_state_mutex** 时调用。fail-safe 策略（D5）：
    ///          - 无事件（ev 为空）→ 保持 Default，reason="no_record"，**必须显式告知**
    ///          - 值非法 → 回退 Plan（从严），reason="invalid_value"
    ///          - 正常 → 恢复全部三态，restored=true
    PermissionRestoreResult restore_permission_locked(
        const std::optional<agent::session::PermissionEvent>& ev);

    std::unique_ptr<ICompletionProvider> m_provider;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    /// @brief 系统提示词重建回调（方案 A，由 m_state_mutex 保护）
    /// @details 模式切换时按目标模式重建提示词（极简只拼白名单工具说明）；nullptr 不重建。
    std::function<std::string(tool::SessionMode)> m_system_prompt_builder;
    std::string m_session_id;  ///< 会话标识（switch_session 可变更，由 m_state_mutex 保护）
    std::string m_cwd;  ///< 会话启动时的工作目录（构造时捕获，注入到 ReActLoop）

    // #45：会话级权限模式（两态 UI：Default/Plan/Bypass，由 m_state_mutex 保护）
    tool::PermissionMode m_permission_mode{tool::PermissionMode::Default};
    ///< 进入 Plan 前的原模式（Plan 退出恢复用，由 m_state_mutex 保护）
    tool::PermissionMode m_permission_mode_before_plan{tool::PermissionMode::Default};

    // 会话工作模式（标准 / 计划 / 极简），由 m_state_mutex 保护
    tool::SessionMode m_session_mode{tool::SessionMode::Standard};

    /// @brief #87：是否在 Plan 模式中（ReActLoop::m_in_plan_mode 的会话级镜像）
    /// @details 由 ReActLoop 回写（EnterPlanMode/ExitPlanMode 工具回调）与 set_session_mode
    ///          维护，随 permission 事件一起持久化。
    bool m_in_plan_mode{false};

    /// @brief #87：最近一次 switch_session 的权限恢复结果（由 m_state_mutex 保护）
    PermissionRestoreResult m_last_permission_restore;
    std::atomic<bool> m_generating{false};

    // DS_CACHE M-3：移除 m_cache_hit_total/m_cache_miss_total 死代码
    // （TUI 使用 token_stats_model.h 自有的累计器，ChatSession 侧为死代码）

    // 上一轮前缀形状（用于本轮对比，诊断缓存劣化归因）
    // 由 m_state_mutex 保护（与 system_prompt / tools 同步更新）
    PrefixShape m_last_prefix_shape;

    // DS_CACHE H-3：跨 turn 持久化的压缩器（卡死守卫/rewrite_version 跨 turn 累积）
    // 由 m_state_mutex 保护（与 messages 生命周期同步）
    CacheAwareCompactor m_compactor;

    // D-1：任务管理器引用（非拥有，构造注入；ChatSession 不可移动，引用安全）
    std::reference_wrapper<ITaskManager> m_task_manager;

    // D-1：事件总线引用（非拥有，构造注入）
    std::reference_wrapper<IEventBus> m_event_bus;

    // C-1：配置管理器引用（非拥有，构造注入）
    std::reference_wrapper<IConfigManager> m_config_manager;

    // 工具注册表（可选，为空时不启用 function calling）
    std::shared_ptr<tool::ToolRegistry> m_tool_registry;

    // conditional skills：命令注册表（激活匹配用，可选）
    std::shared_ptr<command::CommandRegistry> m_command_registry;

    // #56 方案 D：父会话全局 MCP 连接管理器（AgentTool 子 Agent mcpServers 引用复用来源）
    std::shared_ptr<mcp::McpClientManager> m_mcp_manager;

    // conditional skills：touch 路径收集器（会话级累积）+ 已激活 skill 名（避免重复注入）
    skill::TouchCollector m_touch_collector;
    std::unordered_set<std::string> m_activated_skills;

    // 宿主文件索引失效回调（FileWriteTool 写文件后调用，由 m_state_mutex 保护）
    std::function<void()> m_file_index_invalidator;

    // 项目会话恢复：JSONL 持久化（可选，设置后每条消息实时追加）
    std::shared_ptr<agent::session::SessionStore> m_session_store;

    // #50 会话级 HookManager（SessionStart / SessionEnd 事件；装配期由 make_hook_manager 构建）
    std::shared_ptr<agent::hook::HookManager> m_hooks;
    // 会话启停 hook 单次触发守卫（避免 configure_session_store/析构的多次调用重复触发）
    bool m_session_start_hook_fired = false;
    bool m_session_end_hook_fired = false;

    // 懒创建 SessionStore 配置（首条 user 消息时才创建文件）
    bool m_store_configured = false;
    std::string m_store_project_dir;
    std::string m_store_cwd;
    std::string m_store_model;
    std::string m_store_git_branch;

    // system_prompt 事件记录状态（由 m_state_mutex 保护）
    bool m_system_prompt_recorded = false;  ///< 当前提示词是否已落盘
    bool m_pending_system_prompt = false;   ///< store 未创建时待补写的 initial 快照

    // H-3：重试策略统一由 HttpRetryPolicy 管理
    HttpRetryPolicy m_retry_policy;

    // 中断事件订阅
    EventToken m_interrupt_token;

    // 子 Agent 事件持久化订阅（progress/completed）
    EventToken m_sub_progress_token;
    EventToken m_sub_completed_token;

    /// @brief #54：Plan Mode V2 协调器（会话语义级，五阶段 + 并行 explore 编排）
    /// @details 由构造函数创建并接线事件订阅；宿主可通过 accessor 读取阶段与产物。
    std::unique_ptr<plan::PlanCoordinator> m_plan_coordinator;
    /// #54 Plan Mode V2 事件订阅令牌（Enter/Exit/子 Agent 完成）
    EventToken m_plan_enter_token;
    EventToken m_plan_exit_token;
    EventToken m_plan_sub_completed_token;

    // 并发控制：保护 m_messages / m_system_prompt / m_tool_registry / m_current_task
    mutable std::mutex m_state_mutex;
    std::shared_ptr<Task> m_current_task;  // 跟踪当前后台任务，用于析构等待
    std::condition_variable m_task_cv;
    /// 本会话已分发的后台任务 id（P1-1 定向取消；受 m_state_mutex 保护）
    std::vector<std::string> m_background_task_ids;

    // ---- 消息队列（模型忙碌时缓存用户输入）----
    /// 排队消息（FIFO；受 m_queue_mutex 保护）
    std::deque<QueuedMessageItem> m_pending_queue;
    mutable std::mutex m_queue_mutex;
    /// 冲刷请求（Ctrl+Enter 置位，工具轮边界注入后复位；原子读避免持锁）
    std::atomic<bool> m_flush_requested{false};
};

}  // namespace agent
