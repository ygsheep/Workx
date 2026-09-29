/**
 * @file executor.h
 * @brief 命令执行器接口
 * @details 解析用户输入、查找命令、执行命令并返回结果
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <optional>
#include "command.h"
#include "agent/command/inclaude/registry.h"
#include "types.h"

namespace agent::command {

/// 命令执行结果（扩展版，包含元信息）
struct ExecutorResult {
    CommandResult result;
    std::string command_name = {};
    bool should_query{false};  ///< 是否需要后续查询模型
    // TODO: chain input not yet implemented — 字段保留待后续实现
    std::optional<std::string> next_input = std::nullopt;
    bool submit_next_input{false};
};

/// 命令执行器 — 对应 processSlashCommand.tsx 的核心逻辑
///
/// 负责解析用户输入、查找命令、执行命令并返回结果。
class CommandExecutor {
   public:
    explicit CommandExecutor(std::shared_ptr<CommandRegistry> registry);

    /// 解析并执行命令
    /// @param input 用户输入（如 "/help" 或 "/init project"）
    /// @param ctx 命令执行上下文
    /// @return 执行结果（包含文本输出和是否需要后续查询）
    ExecutorResult execute(const std::string& input, const CommandContext& ctx) const;

    /// 解析命令名称和参数
    /// @return (command_name, args)
    static std::pair<std::string, std::string> parse(const std::string& input);

   private:
    std::shared_ptr<CommandRegistry> registry_;
};

}  // namespace agent::command
