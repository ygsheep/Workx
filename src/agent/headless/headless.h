/**
 * @file headless.h
 * @brief Headless（非交互）执行模式入口（Issue #77）
 * @details 复用 factory 的会话装配（create_backend / register_builtin_tools /
 *          build_system_prompt），直接同步构造 ReActLoop 跑一条任务并返回结构化结果。
 *          不依赖 TUI / 向导 / 文件索引 / Island，供脚本、CI、自动化评测驱动。
 * @version 1.0.0
 * @date 2026-09
 */

#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace agent {

class IConfigManager;
class ITaskManager;
class IEventBus;
class ICompletionProvider;

/// @brief Headless 执行选项（由 CLI 参数解析而来）
struct HeadlessOptions {
    std::string task;              ///< 任务文本（-p 参数，或 "-" 表示从 stdin 读）
    bool read_from_stdin = false;  ///< 任务是否从 stdin 读取
    std::string output_format = "text";  ///< text / json / stream-json
    /// @brief 权限模式（accept-edits / bypass-permissions / default）
    std::string permission_mode;
    /// @brief #126：显式声明验证目标（tests_pass / build_clean / cmd:<command> / ...）
    /// @details 非空时覆盖配置与 `WORKX_GOAL`。评测跑在别人准备的目录里常常没有任何
    ///          项目标记文件，目标探测会返回 None、门禁随之完全不介入；
    ///          有这一项就能按题面指定"用什么命令算通过"。
    std::string goal;
};

/// @brief Headless 执行结果
struct HeadlessResult {
    int exit_code = 0;  ///< 语义化退出码：0 成功 / 1 任务失败 / 2 参数配置错误 / 3 预算中断
    std::string output;  ///< 写往 stdout 的内容（text 或 json）
};

/// @brief 运行 headless 单次执行
///
/// @details 装配流程：
///   1. 从 cfg 解析 provider → create_backend
///   2. register_builtin_tools 建工具集
///   3. build_system_prompt 拼系统提示词
///   4. 构造 ReActLoop，同步 run() 一条 user 任务
///   5. 按 output_format 序列化结果到 output
///
/// @param cfg 配置管理器
/// @param task_manager 任务管理器（BashTool 后台任务 DI）
/// @param event_bus 事件总线（工具事件发布 DI）
/// @param opts headless 选项
/// @return 执行结果（含退出码与 stdout 文本）
HeadlessResult run_headless(IConfigManager& cfg, ITaskManager& task_manager, IEventBus& event_bus,
                            const HeadlessOptions& opts);

/// @brief 使用注入的后端运行 headless（Issue #77 测试专用入口）
///
/// @details 与 run_headless 完全相同，仅跳过 provider 解析与 remote_url 校验，
///          直接使用调用方提供的后端 —— 使单元测试能注入 MockCompletionProvider
///          驱动主流程（否则只能依赖真实 API 配置，主流程不可测）。
///          除后端来源外，其余分支（权限模式 / 工具注册 / 输出序列化 / 退出码）共用同一实现。
///
/// @note 不接收 IEventBus：headless 从不向工具注入 event_bus（build_loop 固定传
///       nullptr，AskUserTool 因此自动拒绝提问，避免无人值守阻塞），故该形参无意义。
///
/// @param provider 后端指针，**不接管所有权**，生命周期由调用方保证
/// @return 执行结果；provider 为 nullptr 时 exit_code == 2
HeadlessResult run_headless_with_provider(IConfigManager& cfg, ITaskManager& task_manager,
                                          const HeadlessOptions& opts,
                                          ICompletionProvider* provider);

}  // namespace agent
