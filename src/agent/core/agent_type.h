/**
 * @file agent_type.h
 * @brief AgentType 枚举 + 字符串解析（#33 类型体系 / 里程碑 0.6.x）
 * @details 将 agent.active（历史为 skill 过滤字符串）升级为 Agent 模式路由。
 *          保留 skill 过滤兼容：非空字符串仍参与 skill 匹配，（新）按别名
 *          前缀解析为 AgentType 供 QueryEngine 路由。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <string_view>

namespace agent {

/// @brief Agent 类型（里程碑 0.6.x：有类型/目标/环境感知的多模式体系）
enum class AgentType {
    Unknown = 0,  ///< 未识别 / 空
    ReAct,        ///< 通用交互对话（现状，默认）
    GoalGuarded,  ///< #31 目标驱动，验到成功为止（别名：goal-guarded / verify）
    Planner,      ///< #33 只读规划
    Executor,     ///< #33 方案确认后执行
    Coordinator,  ///< #33 任务编排（依赖 AgentTool）
    Researcher,   ///< #33 收集→对比→总结
    Reviewer,     ///< #33 只读审查
    Batch,        ///< #32 同构输入并行
    Watch,        ///< #32 文件/事件监控
    Script,       ///< #32 确定性脚本执行（无需 LLM）
    Background,   ///< 长时运行：整条请求转后台，不阻塞主对话，事件通知
};

/// @brief 将 agent.active 字符串解析为 AgentType
///
/// 规则（前缀匹配，忽略大小写与首尾空白）：
///   - 空 / "react" / "explore"  → ReAct（explore 为 #33 角色别名）
///   - "goal-guarded" / "goalguarded" / "verify" → GoalGuarded
///   - "planner" → Planner；"executor" → Executor；"coordinator" → Coordinator
///   - "researcher"/"research" → Researcher；"reviewer" → Reviewer
///   - "batch" → Batch；"watch" / "watcher" → Watch
///   - "script" / "script-agent" → Script
/// 其它未知串 → Unknown（不抛异常，调用方决定回退默认 ReAct）
///
/// @param s agent.active 配置值（可为空）
/// @return 解析出的 AgentType
AgentType parse_agent_type(std::string_view s) noexcept;

/// @brief AgentType → 规范化字符串（用于日志/配置回写）
std::string_view to_string(AgentType type) noexcept;

/// @brief 是否为"通用/已落地"类型（Unknown/ReAct/GoalGuarded 之外均视为未实现占位）
/// @details 仅用于日志提示，不参与路由正确性
bool is_implemented(AgentType type) noexcept;

}  // namespace agent