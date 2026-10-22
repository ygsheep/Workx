/**
 * @file mode_agent_common.h
 * @brief 多模式 Agent 公共数据结构与纯函数（#32 / 里程碑 0.6.x）
 * @details 为 ScriptAgent / BatchAgent / WatchAgent 共享的基础设施：
 *          规格解析、命令物化（{item} 替换 + shell 引用）、glob 展开、
 *          快照签名（watch 变化检测）、白名单守卫执行。全部为无副作用纯函数，
 *          便于单测；命令安全统一走 verdict.h 的 guard_command 白名单。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

#include "agent/core/goal_verdict.h"  // AgentGoal
#include "core/export.h"
#include "core/process/exec_output.h"  // ExecOutput

namespace agent {

/// @brief BatchAgent 运行规格（由 AgentGoal::Type==Batch 解析而来）
struct WORKX_API BatchSpec {
    std::string glob;          ///< 输入匹配模式（相对 cwd），空默认 "**/*"
    std::string cmd_template;  ///< 命令模板（含 {item} 占位，逐条物化）
    size_t concurrency = 1;    ///< 并行度（>=1，超上限会截断为 item 数）
};

/// @brief WatchAgent 运行规格（由 AgentGoal::Type==Watch 解析而来）
struct WORKX_API WatchSpec {
    std::string path;          ///< 监控目录（相对/绝对，空默认 cwd）
    std::string glob;          ///< 目录内过滤模式（空 = 监控整目录）
    std::string cmd_template;  ///< 变化触发命令模板（{item} 占位）
    int max_polls = 1;         ///< 轮询次数（首轮仅建立基线）
    int interval_ms = 0;       ///< 两次轮询间隔（ms）
};

/// @brief 从 AgentGoal 解析 BatchAgent 规格
/// @note 返回规格；command 为空时 caller 决定报错
WORKX_API BatchSpec parse_batch_spec(const AgentGoal& goal) noexcept;

/// @brief 从 AgentGoal 解析 WatchAgent 规格
WORKX_API WatchSpec parse_watch_spec(const AgentGoal& goal) noexcept;

/// @brief 将命令模板中的 {item} 占位物化为"shell 安全引用"的实参
/// @param tmpl 模板（可含任意个 {item}）
/// @param item 待替换的实例（通常为文件相对路径）
/// @return 物化后的完整命令串；若 item 含 shell 敏感字符无法安全引用则返回空
///         （调用方应跳过该条并提示，而非注入）
WORKX_API std::string materialize_cmd(const std::string& tmpl, const std::string& item) noexcept;

/// @brief 在 cwd 下按 glob 展开匹配的相对路径列表（正斜杠形式，名字排序）
/// @details 复用 GlobTool 的 glob_match 语义（* 单层 / ** 递归 / ? 单字符）；
///          遍历不跟随 symlink 出逃（recursive_directory_iterator 相对 cwd）。
/// @param cwd   搜索根目录
/// @param pattern 相对 glob 模式；空 = "**/*"
/// @param err   [out] 出错说明（成功置空）
/// @return 匹配的相对路径（正斜杠、升序）
WORKX_API std::vector<std::string> expand_glob_cwd(const std::string& cwd,
                                                   const std::string& pattern, std::string* err);

/// @brief 计算一组相对路径在 cwd 下的内容快照签名（watch 变化检测）
/// @details 对每个存在的文件拼接 "rel|size|mtime"。目录本身不计（只看文件内容）。
/// @param cwd    根目录
/// @param rels   相对路径列表（glob 展开结果）
/// @return 快照签名字符串（任意两次一致的签名 → 内容未变）
WORKX_API std::string snapshot_signature(const std::string& cwd,
                                         const std::vector<std::string>& rels);

/// @brief 白名单守卫执行一条命令并捕获完整输出
/// @param cmd  命令串（先经 guard_command 校验，未过则 rejected=true 不执行）
/// @param cwd  工作目录
/// @param rejected [out] 被白名单拦截置 true（此时 out 无效）
/// @return 执行结果（含 stdout/stderr/exit_code）；若 rejected 则是默认值
/// @note 用 cmd.exe /d /s /c（Win）或 sh -c（POSIX）包装，对齐 verdict.cpp 做法
WORKX_API process::ExecOutput run_whitelisted(const std::string& cmd, const std::string& cwd,
                                              bool* rejected);

}  // namespace agent