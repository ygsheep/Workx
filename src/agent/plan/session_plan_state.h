/**
 * @file session_plan_state.h
 * @brief Plan Mode V2（#54）会话级规划状态数据
 * @details 定义五阶段流转所需的阶段枚举、explore 发现、结构化规划产物（PlanArtifact）。
 *          纯数据头（无 I/O、无 LLM 依赖），可被 PlanCoordinator 与 ChatSession 共同消费，
 *          并支持 JSON 序列化以便 /resume 恢复阶段。
 * @version 1.0.0
 * @date 2026-09
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent::plan {

/// @brief Plan Mode V2 五阶段（+ 空转）
/// @details idle → interview → exploring → planning → awaiting_approval → done
///          rejected 时从 awaiting_approval 回到 interview（修订重试）。
enum class PlanStage {
    Idle,              ///< 未进入规划流程
    Interview,         ///< 阶段 1：澄清需求、收集约束
    Exploring,         ///< 阶段 2：多 explore agent 并行扫描
    Planning,          ///< 阶段 3：plan agent 综合产出实施计划
    AwaitingApproval,  ///< 阶段 4：计划给用户确认
    Done               ///< 阶段 5：已批准，进入执行
};

/// @brief 阶段名（人可读，供 UI 展示）
inline std::string_view to_string(PlanStage s) noexcept {
    switch (s) {
        case PlanStage::Idle:
            return "idle";
        case PlanStage::Interview:
            return "interview";
        case PlanStage::Exploring:
            return "exploring";
        case PlanStage::Planning:
            return "planning";
        case PlanStage::AwaitingApproval:
            return "awaiting_approval";
        case PlanStage::Done:
            return "done";
    }
    return "unknown";
}

/// @brief 单个 explore agent 的发现（一个子域的探索结论）
struct ExploreFinding {
    std::string task_id;  ///< 子任务 id（空表示机械合成/测试场景）
    std::string area;     ///< 探索的子域
    std::string summary;  ///< 发现的文字小结
    std::vector<std::string> critical_files;  ///< 该子域涉及的关键文件
};

/// @brief 结构化规划产物（可被后续执行阶段消费）
struct PlanArtifact {
    std::string summary;  ///< 计划概述
    std::vector<std::string>
        critical_files;  ///< "Critical Files for Implementation" 列表（跨子域去重合并）
    std::vector<ExploreFinding> findings;  ///< 全部 explore 发现快照
    nlohmann::json steps;                  ///< 实施步骤：[{action, detail}]
    nlohmann::json risks;                  ///< 风险列表：[string]
    std::string interview_notes;           ///< Interview 阶段收集的约束/需求
    std::string markdown;  ///< 渲染后的方案全文（写入 ~/.workx/plan/plan_<session>.md）
};

/// @brief 判断阶段是否为终态
inline bool is_terminal(PlanStage s) noexcept { return s == PlanStage::Done; }

// ---- JSON 序列化辅助（PlanCoordinator 与测试共用）----
inline PlanStage stage_from_string(std::string_view s) noexcept {
    if (s == "interview") return PlanStage::Interview;
    if (s == "exploring") return PlanStage::Exploring;
    if (s == "planning") return PlanStage::Planning;
    if (s == "awaiting_approval") return PlanStage::AwaitingApproval;
    if (s == "done") return PlanStage::Done;
    return PlanStage::Idle;
}

}  // namespace agent::plan