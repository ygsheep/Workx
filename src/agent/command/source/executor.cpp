/**
 * @file executor.cpp
 * @brief 命令执行器实现
 * @details 命令解析与分发执行的实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/command/inclaude/executor.h"

#include "agent/skill/inclaude/hooks.h"

namespace agent::command {

CommandExecutor::CommandExecutor(std::shared_ptr<CommandRegistry> registry)
    : registry_(std::move(registry)) {}

ExecutorResult CommandExecutor::execute(const std::string& input, const CommandContext& ctx) const {
    auto [command_name, args] = parse(input);

    auto cmd = registry_->find_by_name(command_name);
    if (!cmd) {
        // 前缀匹配回退：精确匹配失败时，若只有一个命令以输入开头则匹配它
        // 例如 /resum → /resume（CLI 常见的命令缩写支持）
        auto candidates = registry_->get_user_invocable_commands();
        std::vector<std::shared_ptr<CommandBase>> matches;
        for (const auto& c : candidates) {
            const auto& name = c->name();
            if (name.size() >= command_name.size() &&
                name.compare(0, command_name.size(), command_name) == 0) {
                matches.push_back(c);
            }
        }
        if (matches.size() == 1) {
            cmd = matches[0];
        } else {
            return {
                .result = CommandResult::error("Command not found: " + command_name),
                .command_name = command_name,
            };
        }
    }

    if (!cmd->is_enabled()) {
        return {
            .result = CommandResult::error("Command is disabled: " + command_name),
            .command_name = command_name,
        };
    }

    // 敏感命令：日志脱敏（后续扩展点）

    CommandResult result;
    bool should_query = false;
    // TODO: CommandResult::Type::Compact not yet handled (depends on agent/compact/ stub).
    //       When a local command returns Compact, the result is currently passed through
    //       unchanged; add explicit Compact dispatch here once compact/ is implemented.

    if (auto* local_cmd = dynamic_cast<LocalCommand*>(cmd.get())) {
        result = local_cmd->call(args, ctx);
    } else if (auto* prompt_cmd = dynamic_cast<PromptCommand*>(cmd.get())) {
        auto blocks = prompt_cmd->generate_prompt(args, ctx);
        std::string prompt_text;
        for (auto& block : blocks) {
            if (!prompt_text.empty()) prompt_text += "\n";
            prompt_text += block.text;
        }
        // PreActivate hooks：用户显式调用时执行，输出并入提示文本
        if (!prompt_cmd->hooks().empty()) {
            const auto hook_lines = skill::run_preactivate_hooks(prompt_cmd->hooks(), ctx.cwd);
            const auto hook_block = skill::format_hook_output(hook_lines);
            if (!hook_block.empty()) {
                prompt_text = "[skill hooks]\n" + hook_block + "\n" + prompt_text;
            }
        }
        result = CommandResult::ok(std::move(prompt_text));
        should_query = true;
    } else {
        result = CommandResult::error("Unknown command type");
    }

    return {
        .result = std::move(result),
        .command_name = command_name,
        .should_query = should_query,
    };
}

std::pair<std::string, std::string> CommandExecutor::parse(const std::string& input) {
    if (input.empty()) {
        return {"", ""};
    }

    size_t start = 0;
    if (input[0] == '/') {
        start = 1;
    }

    size_t space_pos = input.find(' ', start);
    if (space_pos == std::string::npos) {
        return {input.substr(start), ""};
    }

    std::string command_name = input.substr(start, space_pos - start);
    std::string args = input.substr(space_pos + 1);
    return {command_name, args};
}

}  // namespace agent::command
