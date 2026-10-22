/**
 * @file registry.h
 * @brief 命令注册表接口
 * @details 管理所有可用命令，支持按名查找和分类过滤
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "command.h"

namespace agent::command {

/// 命令注册表 — 对应 commands.ts 的 getCommands() / getCommand()
///
/// 管理所有可用命令，支持按名查找和分类过滤。
class CommandRegistry {
   public:
    /// 注册一个命令
    void register_command(std::shared_ptr<CommandBase> cmd);

    /// 按名查找命令（精确匹配）
    std::shared_ptr<CommandBase> find_by_name(const std::string& name) const;

    /// 获取所有用户可调用命令（用于命令面板）
    std::vector<std::shared_ptr<CommandBase>> get_user_invocable_commands() const;

    /// 获取指定类型的命令
    std::vector<std::shared_ptr<CommandBase>> get_by_type(const std::string& type) const;

    /// 检查命令是否存在
    bool exists(const std::string& name) const;

    /// 获取命令总数
    size_t size() const;

   private:
    std::vector<std::shared_ptr<CommandBase>> commands_;
    std::unordered_map<std::string, std::shared_ptr<CommandBase>> name_index_;
};

}  // namespace agent::command
