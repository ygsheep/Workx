/**
 * @file plan_coordinator.h
 * @brief Plan Mode V2（#54）会话语义级协调器
 * @details 驱动 interview → exploring → planning → awaiting_approval → done 五阶段流转，
 *          并协调多 explore agent 的并行编排与 plan 综合。
 *          本类是纯状态机 + 数据聚合：
 *            - 不持有 IEventBus，不自行订阅事件（事件驱动由宿主 ChatSession 完成，
 *              宿主订阅子 Agent 完成事件后回调方法）；
 *            - 通过注入的 explore_runner / plan_runner 解耦真实 LLM 调用，
 *              便于单元测试注入 mock 校验阶段流转与并行计数。
 * @version 1.0.0
 * @date 2026-09
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "agent/plan/session_plan_state.h"
#include "core/config/config_manager.h"

namespace agent::plan {

/// @brief Plan Mode V2 协调器
class PlanCoordinator {
   public:
    /// @brief 并行 explore 启动回调（宿主注入；供测试注入 mock）
    /// @param task_id 本次 explore 任务的唯一任务 id（供宿主回映 SubAgentCompleted）
    /// @param area 探索子域（人可读标签）
    /// @param prompt 该 explore agent 的系统提示词
    using ExploreRunner = std::function<void(const std::string& task_id, const std::string& area,
                                             const std::string& prompt)>;

    /// @brief plan 综合回调（宿主注入；同步返回结构化产物）
    /// @details 输入全部 explore 发现 + interview 约束，输出规划产物。
    ///          为空时由本类退化为机械合成（从 findings 聚合 + 渲染 markdown）。
    using PlanRunner =
        std::function<PlanArtifact(const std::vector<ExploreFinding>&, const std::string& notes)>;

    explicit PlanCoordinator(IConfigManager& cfg);

    // ---- 配置读取（读取 plan.*）----
    bool auto_enabled() const;
    bool interview_enabled() const;
    int explore_agent_count() const;
    std::string explore_areas() const;  ///< 逗号分隔子域串（可为空）

    // ---- 五个阶段的驱动方法（宿主按事件回调调用）----

    /// @brief 阶段 0→1：进入规划流程
    /// @details 复位上次规划状态。若 auto 开启且 interview 关闭则直接进入探索。
    void begin_plan(const std::string& reason);

    /// @brief 阶段 1→2：提交 Interview 收集的约束/需求后开始并行探索
    /// @details interview 关闭时 begin_plan 已直通探索；本方法供 interview 开启路径。
    void set_interview_notes(const std::string& notes);

    /// @brief 阶段 2：主动启动探索（通常由 begin_plan / set_interview_notes 间接调用）
    /// @details 按 explore_agent_count 逐个调用 explore_runner（宿主据此并行启动）。
    void start_explore();

    /// @brief 单个 explore 任务完成（宿主在 SubAgentCompleted 时调用）
    /// @details 记录发现；全部预期任务收尾后自动进入 planning（调用 plan_runner）。
    void on_explore_task_done(const std::string& task_id, const std::string& result,
                              bool was_error);

    /// @brief 阶段 3：plan 综合完成，进入待确认
    /// @details 通常由 complete_explore 内部调用 plan_runner 产出；宿主也可直接提交。
    void submit_plan(PlanArtifact artifact);

    /// @brief #54：审批时消费结构化关键文件
    /// @details 事件携带的 critical_files 非空且产物缺省时并入产物，供执行阶段读取。
    void set_critical_files(std::vector<std::string> files);

    /// @brief 阶段 4→5：用户批准 / 驳回
    /// @param approved true=批准进入 Done；false=回到 Interview 修订
    void set_approved(bool approved);

    /// @brief 强制收尾为 Done（Bypass 等跳过确认场景）
    void finish();

    // ---- 状态查询 ----
    PlanStage stage() const { return m_stage; }
    const std::vector<ExploreFinding>& findings() const { return m_findings; }
    const PlanArtifact& artifact() const { return m_artifact; }
    const std::string& reason() const { return m_reason; }
    const std::string& context() const { return m_context; }
    const std::vector<std::string>& active_task_ids() const { return m_active_tasks; }
    /// @brief #54：本轮规划序号（begin_plan 每次自增，explore task_id 借此跨轮全局唯一）
    std::size_t plan_cycle() const { return m_plan_cycle; }

    // runner 注入
    void set_explore_runner(ExploreRunner r) { m_explore_runner = std::move(r); }
    void set_plan_runner(PlanRunner r) { m_plan_runner = std::move(r); }

    // ---- JSON 序列化（/resume 恢复阶段）----
    nlohmann::json serialize() const;
    void deserialize(const nlohmann::json& j);

   private:
    void transition(PlanStage s);
    void complete_explore_if_all_done();
    /// @brief 生成 (area, prompt) 探索子域列表（探索 agent 数 = explore_agent_count）
    std::vector<std::pair<std::string, std::string>> build_explore_prompts(
        const std::string& reason) const;
    /// @brief 无 plan_runner 时的机械合成（聚合 findings + 渲染 markdown）
    PlanArtifact synthesize_mechanically() const;
    /// @brief 渲染方案 markdown 全文
    std::string render_markdown(const PlanArtifact& a) const;

    IConfigManager& m_cfg;
    PlanStage m_stage{PlanStage::Idle};
    std::string m_reason;
    std::string m_context;  ///< interview 约束/需求
    std::vector<ExploreFinding> m_findings;
    std::vector<std::string> m_active_tasks;  ///< 尚在运行的 explore 任务 id
    std::vector<std::string> m_explore_areas;  ///< #54：本轮 explore 启动顺序对应的子域列表
    std::size_t m_explore_launched =
        0;  ///< 归档用启动计数（写入 serialize，供恢复参考；重入防护由 stage 承担）
    std::size_t m_plan_cycle = 0;  ///< #54：规划轮次（begin_plan 自增，explore id 跨轮唯一）
    PlanArtifact m_artifact;

    ExploreRunner m_explore_runner;
    PlanRunner m_plan_runner;
};

}  // namespace agent::plan