/**
 * @file builtins.cpp
 * @brief FTXUI TUI 内置命令注册实现（B2 统一命令）
 * @details 命令定义集中在 agent 侧 CommandRegistry；App 仅作为执行入口。
 *          保持命令可搜索关键词（中英文）供命令面板消费。
 */

#include "command/builtins.h"

#include <string>

#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/types.h"
#include "theme/strings.h"

namespace ftxtui {

using agent::command::CommandContext;
using agent::command::CommandRegistry;
using agent::command::CommandResult;
using agent::command::LocalCommand;

void register_ftx_builtins(CommandRegistry& registry, const FtuiCommandCallbacks& cb) {
    auto help_cmd = agent::command::make_local_command("help", std::string(str::kCmdHelpDesc));
    help_cmd->set_argument_hint("/help");
    help_cmd->set_call(
        [reg_ptr = &registry](const std::string&, const CommandContext&) -> CommandResult {
            std::string output = std::string(str::kHelpIntro);
            for (const auto& cmd : reg_ptr->get_user_invocable_commands()) {
                output += "  /" + cmd->name();
                if (!cmd->description().empty()) output += " - " + cmd->description();
                output += "\n";
            }
            return CommandResult::ok(std::move(output));
        });
    registry.register_command(help_cmd);

    auto exit_cmd = agent::command::make_local_command("exit", std::string(str::kCmdExitDesc));
    exit_cmd->set_argument_hint("/exit");
    exit_cmd->set_call(
        [on_exit = cb.on_exit](const std::string&, const CommandContext&) -> CommandResult {
            if (on_exit) on_exit();
            return CommandResult::ok("");
        });
    registry.register_command(exit_cmd);

    // quit：exit 别名
    auto quit_cmd = agent::command::make_local_command("quit", std::string(str::kCmdQuitDesc));
    quit_cmd->set_argument_hint("/quit");
    quit_cmd->set_call(
        [on_exit = cb.on_exit](const std::string&, const CommandContext&) -> CommandResult {
            if (on_exit) on_exit();
            return CommandResult::ok("");
        });
    registry.register_command(quit_cmd);

    auto clear_cmd = agent::command::make_local_command("clear", std::string(str::kCmdClearDesc));
    clear_cmd->set_argument_hint("/clear");
    clear_cmd->set_call(
        [on_clear = cb.on_clear](const std::string&, const CommandContext&) -> CommandResult {
            if (on_clear) on_clear();
            return CommandResult::ok("");
        });
    registry.register_command(clear_cmd);

    auto new_cmd = agent::command::make_local_command("new", std::string(str::kCmdNewDesc));
    new_cmd->set_argument_hint("/new");
    new_cmd->set_call(
        [on_new = cb.on_new](const std::string&, const CommandContext&) -> CommandResult {
            if (on_new) on_new();
            return CommandResult::ok("");
        });
    registry.register_command(new_cmd);

    auto compact_cmd =
        agent::command::make_local_command("compact", std::string(str::kCmdCompactDesc));
    compact_cmd->set_argument_hint("/compact");
    compact_cmd->set_call(
        [on_compact = cb.on_compact](const std::string&, const CommandContext&) -> CommandResult {
            if (on_compact) on_compact();
            return CommandResult::ok("");
        });
    registry.register_command(compact_cmd);

    auto model_cmd = agent::command::make_local_command("model", std::string(str::kCmdModelDesc));
    model_cmd->set_argument_hint("/model");
    model_cmd->set_call([on_model_select = cb.on_model_select](
                            const std::string&, const CommandContext&) -> CommandResult {
        if (on_model_select) on_model_select();
        return CommandResult::ok("");
    });
    registry.register_command(model_cmd);

    auto provider_cmd =
        agent::command::make_local_command("provider", std::string(str::kCmdProviderDesc));
    provider_cmd->set_argument_hint("/provider");
    provider_cmd->set_call([on_provider_select = cb.on_provider_select](
                               const std::string&, const CommandContext&) -> CommandResult {
        if (on_provider_select) on_provider_select();
        return CommandResult::ok("");
    });
    registry.register_command(provider_cmd);

    auto resume_cmd =
        agent::command::make_local_command("resume", std::string(str::kCmdResumeDesc));
    resume_cmd->set_argument_hint("/resume");
    resume_cmd->set_call([on_resume = cb.on_resume](const std::string& args,
                                                    const CommandContext&) -> CommandResult {
        if (on_resume) on_resume(args);
        return CommandResult::ok("");
    });
    registry.register_command(resume_cmd);

    auto rename_cmd =
        agent::command::make_local_command("rename", std::string(str::kCmdRenameDesc));
    rename_cmd->set_argument_hint("/rename <title>");
    rename_cmd->set_call([on_rename = cb.on_rename](const std::string& args,
                                                    const CommandContext&) -> CommandResult {
        if (on_rename) on_rename(args);
        return CommandResult::ok("");
    });
    registry.register_command(rename_cmd);

    auto view_cmd = agent::command::make_local_command("view", std::string(str::kCmdViewDesc));
    view_cmd->set_argument_hint("/view <file>");
    view_cmd->set_call(
        [on_view = cb.on_view](const std::string& args, const CommandContext&) -> CommandResult {
            if (on_view) on_view(args);
            return CommandResult::ok("");
        });
    registry.register_command(view_cmd);

    auto edit_cmd = agent::command::make_local_command("edit", std::string(str::kCmdEditDesc));
    edit_cmd->set_argument_hint("/edit <file>");
    edit_cmd->set_call(
        [on_edit = cb.on_edit](const std::string& args, const CommandContext&) -> CommandResult {
            if (on_edit) on_edit(args);
            return CommandResult::ok("");
        });
    registry.register_command(edit_cmd);

    auto nvim_cmd = agent::command::make_local_command("nvim", std::string(str::kCmdNvimDesc));
    nvim_cmd->set_argument_hint("/nvim [<file>]");
    nvim_cmd->set_call(
        [on_nvim = cb.on_nvim](const std::string& args, const CommandContext&) -> CommandResult {
            if (on_nvim) on_nvim(args);
            return CommandResult::ok("");
        });
    registry.register_command(nvim_cmd);

    // /Test: 前缀测试命令：弹出 AskUser 提问弹窗（便于开发调试 TUI 渲染/交互）
    auto test_askuser_cmd =
        agent::command::make_local_command("Test:askuser", std::string(str::kCmdTestAskUserDesc));
    test_askuser_cmd->set_argument_hint("/Test:askuser");
    test_askuser_cmd->set_call([on_test_askuser = cb.on_test_askuser](
                                   const std::string&, const CommandContext&) -> CommandResult {
        if (on_test_askuser) on_test_askuser();
        return CommandResult::ok("");
    });
    registry.register_command(test_askuser_cmd);
}

}  // namespace ftxtui
