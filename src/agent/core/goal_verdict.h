/**
 * @file goal_verdict.h
 * @brief 目标定义与验证结果（#31 目标导向 Agent / 里程碑 0.6.x）
 * @details 定义用户可声明的"目标"（tests_pass / build_clean / lint_zero /
 *          file_exists / custom_script）以及验证返回状态（Achieved/Pending/Failed）。
 *          验证器具体实现见 verdict.h。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <string_view>

namespace agent {

/// @brief 目标验证整体状态（GoalGuarded 外层环的退出依据）
enum class GoalStatus {
    Unknown = 0,  ///< 未执行目标验证（普通对话）
    Pending,      ///< 目标尚未达成，继续 ReAct
    Achieved,     ///< 目标已达成（成功退出）
    Failed,       ///< 达到最大尝试次数仍未达成（失败退出）
    NotStarted,   ///< 本轮尚未执行过任何验证
};

/// @brief 目标定义（用户通过消息/配置注入）
struct AgentGoal {
    /// @brief 目标类型
    enum Type {
        None = 0,      ///< 无目标守卫（普通对话）
        TestsPass,     ///< 测试全绿
        BuildClean,    ///< 编译零 error（warning 降级可配）
        LintZero,      ///< lint 零告警
        FileExists,    ///< 文件存在
        CustomScript,  ///< 自定义脚本退出码 == 0
        /// #32 多模式 Agent（由对应 Loop 专属执行，不作为 GoalGuarded 的 Verdict）
        Script,  ///< ScriptAgent：确定性命令执行一次（agent.active=script）
        Batch,   ///< BatchAgent：glob 展开同构并行（agent.active=batch）
        Watch,   ///< WatchAgent：路径/事件监控轮询（agent.active=watch）
    };
    Type type = None;
    /// FileExists/Watch：目标路径 / 监控目录；BuildClean/LintZero/TestsPass 留空用默认命令
    std::string path;
    /// CustomScript/Script：命令；Batch：命令模板（{item} 占位）；Watch：触发命令模板
    std::string command;
    /// Batch：glob 模式；Watch：监控目录内的过滤模式（可空 = 监控整目录）
    std::string glob;
    /// 最大尝试轮数（默认 50，可配置）
    int max_attempts = 50;
    /// 每间隔 N 次未达成弹一次"是否继续"询问（<=0 表示不弹）
    int ask_user_every = 0;
    /// Batch：并行度（默认 1）
    int concurrency = 1;
    /// Watch：轮询次数（默认 1）；轮询间隔毫秒（默认 0 = 不等待）
    int watch_polls = 1;
    int watch_interval_ms = 0;

    bool has_goal() const noexcept { return type != None; }
};

/// @brief 解析 agent.goal 配置字符串为 AgentGoal
/// @details 支持：tests_pass / build_clean / lint_zero / file_exists:<path> /
///          cmd:<command>。空串或无法识别 → None（无目标）。
/// @param spec 配置值（可含首尾空白；file_exists:/cmd: 大小写不敏感前缀）
AgentGoal parse_goal(std::string_view spec) noexcept;

}  // namespace agent