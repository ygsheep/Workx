/**
 * @file context.h
 * @brief ToolContext — 工具执行上下文
 * @details 在工具执行过程中传递的运行时信息：会话 ID、工作目录、权限模式、取消信号、
 *          任务管理器、进度回调
 * @version 1.5.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>

namespace agent {

// D-5：前向声明，避免 context.h 强依赖 i_config_manager.h
class IConfigManager;

// 前向声明：ITaskManager 在 agent 命名空间下（非 agent::task）
class ITaskManager;

// 前向声明：IEventBus（用于 AskUserTool 等需要发布事件的工具）
class IEventBus;

// 前向声明：ICompletionProvider（用于 AgentTool 等启动子 Agent 的工具）
class ICompletionProvider;

// 前向声明：HookManager（#50 通用 Hook 事件系统，工具线程触发 PermissionRequest / Subagent* 事件）
namespace hook {
class HookManager;
}

// 前向声明：McpClientManager（#56 方案 D，MCP 连接管理器；完整类型位于
// agent/mcp/mcp_client_manager.h）
namespace mcp {
class McpClientManager;
}

// 前向声明：CommandRegistry（#56 方案 C，命令注册表；完整类型位于
// agent/command/inclaude/registry.h）
namespace command {
class CommandRegistry;
}

namespace tool {

/// @brief 前向声明：ToolRegistry（AgentTool 为子 Agent 构造工具集）
class ToolRegistry;

/// @brief 工具进度回调类型
/// @details 工具执行过程中上报进度文本（如 stdout 增量、心跳信息），
///          由 ReActLoop 注入，最终通过 EventBus 发布到 UI。
/// @param progress_text 进度文本
using ProgressCallback = std::function<void(const std::string& progress_text)>;

/// @brief 权限模式（#36 统一权限决策层）
/// @details 工具 check_permissions 的决策依据，由宿主在构造 ToolContext 时注入：
///          - Default：常规操作放行，危险操作（敏感路径/目录逃逸/敏感命令）AskUser 确认
///          - AcceptEdits：接受编辑类自动放行（CC 兼容占位）
///          - Plan：计划/只读模式，禁止写文件与执行命令
///          - BypassPermissions：完全放行（用户显式授权后）
///          - Strict：#85 严格档 —— 命令按白名单放行，未命中需确认
///            （headless 无确认通道即拒绝）；破坏性命令直接拒绝，不询问。
enum class PermissionMode : uint8_t {
    Default = 0,
    AcceptEdits = 1,
    Plan = 2,
    BypassPermissions = 3,
    Strict = 4,
};

/// @brief 会话工作模式（模式列表：标准 / 计划 / 极简）
/// @details 与权限模式正交：模式是用户侧的顶层选择（状态栏最前位展示），
///          权限（手动审批 / 完全访问）在模式内部独立切换。
///          - Standard：标准模式，全部工具可用
///          - Plan：计划模式，只读规划（进入时联动权限 Plan，退出时恢复）
///          - Minimal：极简模式，仅暴露 Skill/Bash/Read/Write/Edit 五个工具
enum class SessionMode : uint8_t {
    Standard = 0,  ///< 标准模式：全部工具 + 权限独立
    Plan = 1,      ///< 计划模式：只读规划（对齐 PermissionMode::Plan）
    Minimal = 2,   ///< 极简模式：仅 Skill/Bash/Read/Write/Edit
};

/// @brief 极简模式工具白名单（SessionMode::Minimal 下唯一允许调用的工具）
/// @details schema 过滤（LLM 看不到的工具不会调用）与 ToolExecutor 守卫（幻觉
///          工具名直接拒绝）共用此单一来源，避免两处漂移。
inline constexpr const char* kMinimalModeToolNames[] = {
    "Skill", "Bash", "Read", "Write", "Edit",
};
inline constexpr int kMinimalModeToolCount = 5;

/// @brief 判断工具名是否在极简模式白名单内
inline bool is_minimal_mode_tool(std::string_view name) {
    for (int i = 0; i < kMinimalModeToolCount; ++i) {
        if (name == kMinimalModeToolNames[i]) return true;
    }
    return false;
}

/// @brief 工具 touch 回调类型
/// @details 工具执行过程中上报访问过的文件路径（绝对路径），
///          由 ReActLoop 注入，用于 conditional skills 的路径匹配。
/// @param path 访问的文件路径（绝对路径）
using TouchCallback = std::function<void(const std::string& path)>;

/// @brief 文件系统变更回调类型
/// @details 工具写入/删除文件后调用（无参数），由宿主注入，
///          用于通知宿主失效文件索引（如 TUI @ 补全索引）等缓存。
using FileSystemChangedCallback = std::function<void()>;

/// @brief #79：子 Agent 派生被拒的原因（供调用方拼模型可读的错误文案）
enum class SubAgentReject {
    None,        ///< 未拒绝（预约成功）
    BatchLimit,  ///< 单次批量数超过 max_batch
    TotalLimit,  ///< 累计总数超过 max_total
};

/// @brief #79：单次 run 的子 Agent 派生预算（成本护栏）
///
/// @details 由 ReActLoop 在 run() 开始时按配置构造，经 ToolContext 注入工具；
///          AgentTool 派生前调用 try_reserve(n) 原子预约额度，两级护栏：
///          - n > max_batch：单次批量规模超限
///          - 累计 + n > max_total：全程总量超限
///
///          拒绝时 AgentTool 返回结构化错误回灌模型（含上限与剩余额度），
///          由模型自行拆分任务或缩减并行规模，而非静默截断——保留模型的可纠正性。
///
///          线程安全：Action 阶段并行执行多个 tool_use，同一轮可能有多个 AgentTool
///          并发预约，故计数用 atomic + CAS 循环。
///
///          子 Agent 的工具集不含 AgentTool（防递归），预算无需向下传递。
class SubAgentBudget {
   public:
    /// @param max_batch 单次批量上限（<=0 表示不限）
    /// @param max_total 单次 run 累计上限（<=0 表示不限）
    SubAgentBudget(int max_batch, int max_total)
        : max_batch_(max_batch), max_total_(max_total) {}

    /// @brief 尝试预约 n 个子 Agent 额度（成功则计数已扣除）
    /// @param n 本次拟派生数量
    /// @param[out] reject 被拒原因（成功时为 None）
    /// @return true 允许派生；false 拒绝（计数不变）
    bool try_reserve(int n, SubAgentReject& reject) {
        reject = SubAgentReject::None;
        if (n <= 0) return true;
        if (max_batch_ > 0 && n > max_batch_) {
            reject = SubAgentReject::BatchLimit;
            return false;
        }
        if (max_total_ <= 0) {
            launched_.fetch_add(n);
            return true;
        }
        int cur = launched_.load();
        while (true) {
            if (cur + n > max_total_) {
                reject = SubAgentReject::TotalLimit;
                return false;
            }
            if (launched_.compare_exchange_weak(cur, cur + n)) return true;
        }
    }

    int max_batch() const { return max_batch_; }
    int max_total() const { return max_total_; }
    int launched() const { return launched_.load(); }

    /// @brief 剩余可派生额度（不限时返回 -1）
    int remaining() const {
        if (max_total_ <= 0) return -1;
        return std::max(0, max_total_ - launched_.load());
    }

   private:
    int max_batch_;
    int max_total_;
    std::atomic<int> launched_{0};
};

/// @brief 工具执行上下文
///
/// 在工具执行过程中传递的运行时信息：
/// - 当前会话 ID 与请求 ID
/// - 工作目录路径
/// - 权限模式
/// - 中断信号（CancellationToken）
/// - 任务管理器（用于后台任务）
/// - 进度回调
struct ToolContext {
    std::string cwd;         ///< 工作目录
    std::string session_id;  ///< 会话 ID
    std::string request_id;  ///< 请求 ID（每次 turn 生成，审计/缓存链路关联）
    std::string model;       ///< 当前模型名称（来自 provider 配置）
    nlohmann::json options;  ///< 额外选项

    /// @brief #30：git 环境字段（运行时注入，非会话持久化元数据）
    /// @details 由 ReActLoop 在 turn 开始时通过 git 命令探测，供工具做改前
    ///          分支/未提交改动风险提示。全部为空/默认值表示当前不在 git 仓库内。
    std::string git_branch;            ///< 当前分支（非仓库则为空）
    bool git_has_uncommitted = false;  ///< 是否有未提交改动
    std::string git_repo_root;         ///< 仓库根目录（非仓库则为空）

    /// @brief #137：本次 run 的剩余墙钟（毫秒），由 ReActLoop 在每轮执行工具前注入
    /// @details 用于让 shell 工具按剩余预算封顶单次命令超时，避免一条卡住的命令
    ///          独吞整轮预算（实测一条命令跑满 600s 吃掉 900s 题目的 2/3 时钟）。
    ///          -1 表示未注入/不限，此时工具行为与注入前完全一致（默认不启用）。
    int remaining_wall_clock_ms = -1;

    /// @brief #30：只读历史摘要（当前 turn 前用户指令/约束的文本摘要）
    /// @details 由 ReActLoop 在 turn 开始时从对话历史构建（最近若干条 user 消息，
    ///          截断总量），工具只读以感知用户早先约束；不留修改历史的通道。
    std::string history_summary;

    /// @brief 权限模式（#36 权限决策层）
    /// @details 默认 Default；宿主可在构造时注入（如 CLI 的 --bypass-permissions）。
    ///          工具 check_permissions 依据该模式决定放行/确认/拒绝。
    PermissionMode permission_mode{PermissionMode::Default};

    /// @brief 会话工作模式（标准 / 计划 / 极简）
    /// @details 由 ReActLoop 在 turn 开始时注入。ToolExecutor 依据该模式做
    ///          第二道守卫：Minimal 下仅允许白名单工具（幻觉工具名直接拒绝）。
    SessionMode session_mode{SessionMode::Standard};

    /// @brief #79：单次 run 的子 Agent 派生预算（由 ReActLoop 在 run() 开头注入）
    /// @details 可为 nullptr（如子 Agent 自身、或未走 ReActLoop 的测试路径），
    ///          此时 AgentTool 视为不限——但生产路径下 ReActLoop 必定注入非空实例。
    std::shared_ptr<SubAgentBudget> sub_agent_budget;

    /// @brief 权限模式变更回调类型（#28：EnterPlanMode/ExitPlanMode 注入路径）
    /// @details 工具通过 set_permission_mode() 请求模式切换，由宿主（ReActLoop）
    ///          接线更新会话级权限模式。非空时调用，空则忽略（工具仅提示）。
    using PermissionModeChangedCallback = std::function<void(PermissionMode)>;

    /// @brief 权限模式变更回调（可选）
    /// @details 由调用方（ReActLoop）注入，EnterPlanModeTool/ExitPlanModeV2Tool
    ///          调用 set_permission_mode() 时触发。生命周期由 ToolContext 所有者保证。
    PermissionModeChangedCallback on_permission_mode_changed = nullptr;

    /// @brief 请求切换权限模式（#28）
    /// @details 仅当回调非空时生效（宿主接线后才能改变会话级模式）。
    ///          const 语义：仅通过回调间接修改宿主状态，不改自身。
    void set_permission_mode(PermissionMode mode) const {
        if (on_permission_mode_changed) {
            on_permission_mode_changed(mode);
        }
    }

    /// @brief 进入计划模式回调类型（#28 评审 #1/#3）
    /// @details 宿主（ReActLoop）接线并保留进入前的原模式，随后切换为 Plan。
    ///          返回 false 表示拒绝进入：已在 Plan 模式（幂等）或处于
    ///          BypassPermissions（禁止降级，防权限丢失）。工具据此决定是否发布事件。
    using EnterPlanModeCallback = std::function<bool()>;

    /// @brief 进入计划模式回调（可选）
    /// @details nullptr 时 EnterPlanModeTool 回退到 set_permission_mode(Plan)。
    ///          生命周期由 ToolContext 所有者保证。
    EnterPlanModeCallback on_enter_plan_mode = nullptr;

    /// @brief 退出计划模式回调类型（#28 评审 #1）
    /// @details 宿主（ReActLoop）接线：批准退出时恢复进入计划前的原模式，
    ///          而非硬编码回 Default，避免 Bypass/AcceptEdits 等原模式丢失。
    using ExitPlanModeCallback = std::function<void()>;

    /// @brief 退出计划模式回调（可选）
    /// @details nullptr 时 ExitPlanModeV2Tool 回退到 set_permission_mode(Default)。
    ///          生命周期由 ToolContext 所有者保证。
    ExitPlanModeCallback on_exit_plan_mode = nullptr;

    /// @brief 外部取消信号指针（可选）
    /// @details 2.3 修复：由调用方（ReActLoop）传入 should_cancel 的地址，
    ///          使工具能即时感知外部取消请求。nullptr 时回退到内部 cancelled_。
    ///          生命周期由调用方保证（栈变量通常在 ToolContext 之上存活）。
    const std::atomic<bool>* cancel_flag = nullptr;

    /// @brief H-5：配置管理器指针（必填，非拥有）
    /// @details 由调用方（ReActLoop）显式注入，工具通过 config_manager() 访问。
    ///          H-5 移除单例回退：nullptr 时 config_manager() 抛 std::logic_error，
    ///          强制 DI 显式依赖。生命周期由调用方保证（通常为 ChatSession 的成员）。
    IConfigManager* config_manager_ptr = nullptr;

    /// @brief 任务管理器指针（可选，非拥有）
    /// @details 由调用方（ReActLoop）显式注入，工具通过 task_manager() 访问。
    ///          nullptr 时 task_manager() 抛 std::logic_error。
    ///          用于 BashTool 等需要启动后台任务的工具。
    ///          生命周期由调用方保证（通常为 ChatSession 引用的 TaskManager 单例）。
    ITaskManager* task_manager_ptr = nullptr;

    /// @brief 事件总线指针（可选，非拥有）
    /// @details 由调用方（ReActLoop）显式注入，工具通过 event_bus() 访问。
    ///          nullptr 时 event_bus() 抛 std::logic_error。
    ///          用于 AskUserTool 等需要发布事件的工具。
    ///          生命周期由调用方保证（通常为 ChatSession 持有的 EventBus 引用）。
    IEventBus* event_bus_ptr = nullptr;

    /// @brief 推理提供者指针（可选，非拥有）
    /// @details 由调用方（ReActLoop）显式注入（其持有的 ICompletionProvider）。
    ///          AgentTool 等启动子 Agent 的工具依赖它构造子 ReActLoop。
    ///          nullptr 时子 Agent 工具返回错误（无 LLM 可用）。
    ///          生命周期由调用方保证。
    ICompletionProvider* provider_ptr = nullptr;

    /// @brief 工具注册表（可选，非拥有）
    /// @details 由调用方（ReActLoop）显式注入其持有的注册表。
    ///          AgentTool 为子 Agent 构造工具集（get_all_schemas）。
    ///          nullptr 时子 Agent 无工具可用。
    std::shared_ptr<ToolRegistry> tool_registry;

    /// @brief 技能/命令注册表（可选，非拥有，#56 方案 C）
    /// @details 由调用方（ReActLoop）显式注入其持有的 CommandRegistry（bundled + 磁盘技能）。
    ///          AgentTool 按 skill 名预加载全文到子 Agent 初始消息。
    ///          nullptr 时 skill 预加载静默跳过。生命周期由调用方（ChatSession）保证。
    command::CommandRegistry* command_registry_ptr = nullptr;

    /// @brief MCP 连接管理器指针（可选，非拥有，#56 方案 D）
    /// @details 由调用方（ReActLoop / 子 Agent 构造）显式注入。MCPTool 实际自行持有
    ///          manager，本指针供工具在需要时访问当前作用域可用的 MCP server 集合
    ///          （父会话全局 manager 或子 Agent 临时 manager）。nullptr 表示无可用的
    ///          MCP 通道。生命周期由调用方保证。
    mcp::McpClientManager* mcp_manager_ptr = nullptr;

    /// @brief Hook 事件管理器（可选；#50 通用 Hook 事件系统）
    /// @details 由调用方（ReActLoop）注入循环级 HookManager，供工具线程触发
    ///          PermissionRequest / SubagentStart / SubagentStop 事件。
    ///          线程安全：dispatch 内部互斥锁保证并发安全。
    ///          共享语义：持有 shared_ptr 保证工具异步线程执行 dispatch 期间
    ///            HookManager（及其中注册表）不提前析构。
    ///          生命周期：与所属循环（ReActLoop/子 Agent loop）一致。
    std::shared_ptr<agent::hook::HookManager> hook_manager_ptr;

    /// @brief 进度回调（可选）
    /// @details 由调用方（ReActLoop）注入，工具在长任务执行过程中调用以上报进度。
    ///          默认为空（无进度上报）。生命周期由 ToolContext 所有者保证。
    ProgressCallback progress_callback = nullptr;

    /// @brief touch 回调（可选）
    /// @details 由调用方（ReActLoop）注入，工具访问文件时调用以上报路径，
    ///          供 conditional skills 匹配激活。默认为空。生命周期由 ToolContext 所有者保证。
    TouchCallback touch_callback = nullptr;

    /// @brief 文件系统变更回调（可选）
    /// @details 由调用方（ReActLoop）注入，工具写入/删除文件后调用，
    ///          宿主据此失效文件索引等缓存。默认为空（无宿主，no-op）。
    ///          生命周期由 ToolContext 所有者保证。
    FileSystemChangedCallback on_file_system_changed = nullptr;

    /// @brief 解析配置管理器（H-5：nullptr 时抛异常，不再回退单例）
    /// @return IConfigManager 引用
    /// @throws std::logic_error 当 config_manager_ptr == nullptr
    IConfigManager& config_manager() const;

    /// @brief 解析任务管理器
    /// @return ITaskManager 引用
    /// @throws std::logic_error 当 task_manager_ptr == nullptr
    ITaskManager& task_manager() const;

    /// @brief 解析事件总线
    /// @return IEventBus 引用
    /// @throws std::logic_error 当 event_bus_ptr == nullptr
    IEventBus& event_bus() const;

    /// @brief 检查是否已取消
    /// @return 已取消返回 true
    bool is_cancelled() const {
        if (cancel_flag != nullptr) {
            return cancel_flag->load(std::memory_order_acquire);
        }
        return cancelled_.load(std::memory_order_relaxed);
    }

    /// @brief 请求取消（仅当未绑定外部 cancel_flag 时有效）
    void cancel() {
        if (cancel_flag == nullptr) {
            cancelled_.store(true, std::memory_order_relaxed);
        }
        // 绑定外部 cancel_flag 时由外部负责置位，本方法无操作
    }

    /// @brief 上报进度（若 progress_callback 已设置）
    /// @param progress_text 进度文本
    void report_progress(const std::string& progress_text) const {
        if (progress_callback) {
            progress_callback(progress_text);
        }
    }

    /// @brief 上报 touch（若 touch_callback 已设置）
    /// @param path 访问的文件路径（绝对路径）
    void report_touch(const std::string& path) const {
        if (touch_callback) {
            touch_callback(path);
        }
    }

   private:
    std::atomic<bool> cancelled_{false};  ///< 内部取消标志（fallback）
};

}  // namespace tool
}  // namespace agent
