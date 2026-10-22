/**
 * @file registry.cpp
 * @brief 命令注册表实现
 * @details 注册、查找、过滤命令的实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/command/inclaude/registry.h"

#include <algorithm>

namespace agent::command {

void CommandRegistry::register_command(std::shared_ptr<CommandBase> cmd) {
    name_index_[cmd->name()] = cmd;
    commands_.push_back(std::move(cmd));
}

std::shared_ptr<CommandBase> CommandRegistry::find_by_name(const std::string& name) const {
    auto it = name_index_.find(name);
    return it != name_index_.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<CommandBase>> CommandRegistry::get_user_invocable_commands() const {
    std::vector<std::shared_ptr<CommandBase>> result;
    std::copy_if(commands_.begin(), commands_.end(), std::back_inserter(result),
                 [](const auto& cmd) {
                     return cmd->is_enabled() && cmd->is_user_invocable() && !cmd->is_hidden();
                 });
    return result;
}

std::vector<std::shared_ptr<CommandBase>> CommandRegistry::get_by_type(
    const std::string& type) const {
    std::vector<std::shared_ptr<CommandBase>> result;
    std::copy_if(commands_.begin(), commands_.end(), std::back_inserter(result),
                 [&](const auto& cmd) { return cmd->type() == type; });
    return result;
}

bool CommandRegistry::exists(const std::string& name) const { return name_index_.contains(name); }

size_t CommandRegistry::size() const { return commands_.size(); }

}  // namespace agent::command
