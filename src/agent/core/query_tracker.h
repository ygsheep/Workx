/**
 * @file query_tracker.h
 * @brief QueryTracker — 查询调用链追踪（0.6.x 里程碑要求项）
 * @details 轻量记录每次查询的 {agent_type, goal, 验证历史, 终态}，供
 *          QueryEngine::run() 顺序写入；UI/诊断可读取最近 N 条历史展示
 *          "类型 → 目标 → Verdict 进度 → 结果"调用链。
 *          无锁：由 QueryEngine 单线程持有，每轮查询顺序调用 begin/verdict/finish。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent/core/agent_type.h"
#include "agent/core/goal_verdict.h"  // GoalStatus

namespace agent {

/// @brief 一次目标验证结果（GoalGuarded 每轮 check_goal 的记录）
struct VerdictRecord {
    int attempt = 0;                          ///< 1-based 尝试轮数
    GoalStatus status = GoalStatus::Unknown;  ///< 验证终态（Pending/Achieved/Failed）
    std::string detail;  ///< 验证器人可读说明（测试失败数/缺失路径等）
};

/// @brief 一轮查询记录
struct QueryRecord {
    AgentType agent_type = AgentType::Unknown;      ///< 实际执行的 Agent 类型
    std::string goal_spec;                          ///< agent.goal 原文（空 = 普通对话）
    std::vector<VerdictRecord> verdict_history;     ///< 目标验证历史（GoalGuarded 才有）
    GoalStatus final_status = GoalStatus::Unknown;  ///< 终态（仅目标守卫有意义）
    std::string final_answer;                       ///< 最终回复摘要
    int64_t started_ms = 0;                         ///< 本轮起点（steady_clock ms）
    int64_t finished_ms = 0;                        ///< 本轮终点

    bool has_goal() const noexcept { return !goal_spec.empty(); }
};

/// @brief 查询跟踪器（里程碑要求：QueryEngine → queryTracking 调用链）
/// @details 保留最近 m_capacity 条历史（默认 50，环形去头），engine 单线程调用。
class QueryTracker {
   public:
    /// @brief 开始一轮查询（普通对话 goal_spec 传空）
    void begin(AgentType type, std::string goal_spec) {
        m_history.push_back(QueryRecord{});
        auto& cur = m_history.back();
        cur.agent_type = type;
        cur.goal_spec = std::move(goal_spec);
        cur.started_ms = now_ms();
        trim();
    }

    /// @brief 记录一次目标验证结果（仅 GoalGuarded 调用）
    void record_verdict(GoalStatus status, std::string detail) {
        if (m_history.empty()) {
            return;
        }
        auto& cur = m_history.back();
        VerdictRecord v;
        v.attempt = static_cast<int>(cur.verdict_history.size()) + 1;
        v.status = status;
        v.detail = std::move(detail);
        cur.verdict_history.push_back(std::move(v));
    }

    /// @brief 结束本轮查询（普通对话 goal_status 置 Unknown 即可）
    void finish(GoalStatus status, std::string final_answer) {
        if (m_history.empty()) {
            return;
        }
        auto& cur = m_history.back();
        cur.final_status = status;
        cur.final_answer = std::move(final_answer);
        cur.finished_ms = now_ms();
    }

    /// @brief 当前进行中/刚完成的一轮（m_history.back()；empty 时返回默认记录）
    const QueryRecord& current() const {
        static const QueryRecord kEmpty;
        return m_history.empty() ? kEmpty : m_history.back();
    }
    bool has_current() const noexcept { return !m_history.empty(); }

    /// @brief 全部历史（新→旧排列，容量上限 m_capacity）
    const std::vector<QueryRecord>& history() const noexcept { return m_history; }

    void set_capacity(size_t n) {
        m_capacity = n;
        trim();
    }
    size_t capacity() const noexcept { return m_capacity; }

    void clear() { m_history.clear(); }

   private:
    void trim() {
        if (m_history.size() > m_capacity) {
            m_history.erase(m_history.begin(), m_history.begin() + (m_history.size() - m_capacity));
        }
    }
    static int64_t now_ms() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    }

    std::vector<QueryRecord> m_history;
    size_t m_capacity = 50;
};

}  // namespace agent