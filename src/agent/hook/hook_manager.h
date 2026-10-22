/**
 * @file hook_manager.h
 * @brief 通用 Hook 事件系统 — 调度核心（注册表 + dispatch + 执行器门面）
 * @details Issue #50。HookManager 持有已注册的 HookDefinition 列表（按注册
 *          顺序），dispatch(event, ctx) 时先用一次性编译的 HookMatcher 过滤，
 *          再逐条执行 command/http/prompt/agent 类型的 hook，聚合 blockingError /
 *          preventContinuation / message。进度通过 IEventBus 发布可选。
 * @version 1.0.1
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "agent/hook/hook_event.h"
#include "agent/hook/hook_match.h"

namespace agent {
class ICompletionProvider;
class IConfigManager;
class IEventBus;
namespace tool {
class ToolRegistry;
}
}  // namespace agent

namespace agent::hook {

class HookManager;  // 前向声明（make_hook_manager 返回 shared_ptr 需要）

/// @brief 从配置构建 HookManager（读取 hooks.enabled / hooks.definitions）
/// @details 供 QueryEngine（per-query 循环级）与 ChatSession（会话级 SessionStart/End）
///          复用同一套装配逻辑：注册配置中的 hook 定义并注入 provider/event_bus，
///          以及 agent 类型所需的 config_manager / 白名单来源 registry。
///          hooks.enabled 为 false 时返回空 manager（empty()==true，零开销短路）。
/// @param tool_registry 白名单来源（agent 类型从中派生只读子工具集；空则 agent 退化为纯 prompt
/// 判定）
std::shared_ptr<HookManager> make_hook_manager(
    agent::IConfigManager& cfg, std::shared_ptr<agent::tool::ToolRegistry> tool_registry,
    agent::ICompletionProvider* provider, agent::IEventBus* bus);

struct HookEntry {
    HookDefinition def;
    HookMatcher matcher;
    bool consumed = false;   // once 语义：已执行过
    uint64_t run_count = 0;  // 执行次数（调试/日志）

    explicit HookEntry(HookDefinition d) : def(std::move(d)), matcher(def.match) {}
};

/// @brief Hook 生命周期接口。宿主（ReActLoop/ChatSession/AgentTool）持有一个
///        shared_ptr<HookManager> 在各事件点调用 dispatch()。
/// @details 线程安全：注册集中在装配期（启动/会话开始，单线程）；dispatch 可在
///          任意线程并发调用（工具经 std::async 在工作线程触发 PermissionRequest、
///          AgentTool 触发 Subagent*，与主循环 PreToolUse/PostToolUse/Stop 并发）。
///          dispatch 用互斥锁对匹配条目做快照，执行 run_hook 在锁外进行，既保证
///          once/run_count 不被并发重复执行，又不至于让长耗时（LLM/子进程）串行
///          阻塞其他线程。empty()/size() 热路径不加锁，依赖运行期不变量：注册
///          仅在首个 dispatch 之前完成，运行期不增删条目。
class HookManager {
   public:
    /// @brief 注册一个 hook（若同 event+match 已存在则忽略，避免 config+frontmatter 重复）
    void register_hook(HookDefinition def);

    /// @brief 批量注册（清空旧列表）。config 加载 / 会话生命周期切换时调用。
    void register_hooks(std::vector<HookDefinition> defs);

    /// @brief 清除全部 hook（clearSessionHooks 对应）
    void clear();

    /// @brief 调度事件：过滤匹配的 hook 并顺序执行，聚合结果
    /// @param event 事件类型
    /// @param ctx 事件上下文
    /// @return 聚合结果；无条件命中时返回默认（无效）
    HookResult dispatch(HookEvent event, const HookContext& ctx);

    /// @brief 是否注册了任何 hook（空 → 调用方可零开销短路）
    bool empty() const noexcept { return entries_.empty(); }

    /// @brief 已注册 hook 数（调试/UI）
    size_t size() const noexcept { return entries_.size(); }

    /// @brief 注入 LLM provider（prompt/agent 类型需要；非拥有指针）
    /// @note 生命周期约束（M-1）：调用方必须保证 provider 存活期 ≥ 本 HookManager，
    ///       推荐由 QueryEngine 装配期注入（二者同源同生命周期）。
    ///       调用方不得在 dispatch 过程中销毁 provider；违规会导致悬垂指针。
    ///       为 nullptr 时 prompt/agent hook 降级为未就绪消息（不崩溃）。
    void set_provider(agent::ICompletionProvider* provider) noexcept { provider_ = provider; }

    /// @brief 注入事件总线（可选；非拥有指针，用于发布 hook 进度事件）
    /// @note 为 nullptr 时跳过进度事件发布（日志仍正常），测试/无 UI 环境可留空
    void set_event_bus(agent::IEventBus* bus) noexcept { event_bus_ = bus; }

    /// @brief 注入配置管理器（agent 类型子 ReActLoop 需要；非拥有指针）
    /// @note 生命周期与 M-1 的 provider 一致：宿主保证存活期 ≥ 本管理器。
    ///       为 nullptr 时 agent hook 降级为未就绪消息（不崩溃）。
    void set_config_manager(agent::IConfigManager* cfg) noexcept { config_manager_ = cfg; }

    /// @brief 注入白名单来源 registry（agent 类型从中派生只读子工具集）
    /// @details 非拥有性（shared_ptr 持有权归宿主）；为空则 agent 退化为纯 prompt 判定。
    ///          来源 registry 变化时使只读子工具集缓存失效（下次按新来源重建）。
    void set_tool_registry(std::shared_ptr<agent::tool::ToolRegistry> reg) noexcept {
        std::lock_guard<std::mutex> lock(tool_mutex_);
        tool_registry_ = std::move(reg);
        cached_sub_registry_.reset();
    }

   private:
    std::vector<HookEntry> entries_;
    mutable std::mutex mutex_;  ///< 保护 entries_ 的并发访问（dispatch 快照）
    agent::ICompletionProvider* provider_ = nullptr;
    agent::IConfigManager* config_manager_ = nullptr;
    agent::IEventBus* event_bus_ = nullptr;
    std::shared_ptr<agent::tool::ToolRegistry> tool_registry_;  ///< agent 类型白名单来源
    mutable std::mutex tool_mutex_;  ///< 保护 tool_registry_ 与子工具集缓存
    std::shared_ptr<agent::tool::ToolRegistry>
        cached_sub_registry_;  ///< 只读子工具集缓存（M-1 复用，来源稳定时不为空）

    /// @brief 获取只读子工具集（懒构建 + 缓存；来源 registry 稳定时复用）
    /// @details 线程安全：内部加锁，首次构建后缓存返回。调用方持返回值存活期即本次
    ///          run_agent 期间，即使来源 registry 并发替换本快照仍有效。
    std::shared_ptr<agent::tool::ToolRegistry> get_sub_registry();

    /// @brief 执行单条 hook，返回其 HookResult
    HookResult run_hook(const HookEntry& entry, const HookContext& ctx);

    /// @brief 执行附带的输出/结果聚合逻辑（message 等）
    void aggregate(const HookResult& r, HookResult& out) const noexcept;

    // 四种类型执行器
    HookResult run_command(const HookDefinition& def, const HookContext& ctx);
    HookResult run_http(const HookDefinition& def, const HookContext& ctx);
    HookResult run_prompt(const HookDefinition& def, const HookContext& ctx);
    HookResult run_agent(const HookDefinition& def, const HookContext& ctx);
};

}  // namespace agent::hook
