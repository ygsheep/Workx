/**
 * @file types.h
 * @brief agent/input 类型定义
 */

#pragma once
#include <string>
#include <vector>
#include <optional>

namespace agent::input {
/// 解析后的斜杠命令
struct ParsedSlashCommand {
    std::string command_name;
    std::string args;
    bool is_mcp{false};
};

/// 输入类型枚举
enum class InputType {
    Text,          /// 普通文本（发送给LLM）
    SlashCommand,  /// 斜杠命令（/help, /init）
    BashCommand,   /// Bash命令（终端执行）
    Empty,         /// 空输入
};

/// 用户输入解析结果
struct ParsedInput {
    InputType type;
    std::string text;                           /// 原始文本（Text类型）
    std::optional<ParsedSlashCommand> command;  /// 解析后的命令（SlashCommand类型）
    std::vector<std::string> attachments;       /// 文件附件路径
    std::vector<std::string> image_paths;       /// 图片路径
};

/// 输入处理结果
struct ProcessResult {
    bool should_query{false};              /// 是否需要调用LLM
    std::string output_text;               /// 命令执行输出
    std::vector<std::string> messages;     /// 待发送给LLM的消息列表
    std::vector<std::string> image_paths;  /// 图片附件（已转为绝对路径）
    bool is_error{false};                  /// 是否发生错误
};

}  // namespace agent::input