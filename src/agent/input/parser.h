/**
 * @file parser.h
 * @brief agent/input 解析器
 */

#pragma once
#include <string>
#include <vector>
#include <optional>
#include <regex>

#include "types.h"

namespace agent::input {

class InputParser {
   public:
    /// 解析用户输入
    auto parse(const std::string& input) -> ParsedInput {
        std::string trimmed = trim(input);

        if (trimmed.empty()) {
            return {.type = InputType::Empty,
                    .text = {},
                    .command = {},
                    .attachments = {},
                    .image_paths = {}};
        }

        // 检查是否为斜杠命令
        if (trimmed.starts_with('/')) {
            if (auto cmd = parse_slash_command(trimmed)) {
                return {
                    .type = InputType::SlashCommand,
                    .text = {},
                    .command = std::move(cmd),
                    .attachments = {},
                    .image_paths = {},
                };
            }
        }

        // 检查是否为bash命令（以!开头）
        if (trimmed.starts_with('!')) {
            return {
                .type = InputType::BashCommand,
                .text = trimmed.substr(1),
                .command = {},
                .attachments = {},
                .image_paths = {},
            };
        }

        // 普通文本，提取附件引用
        auto [text, attachments, images] = extract_attachments(trimmed);
        return {
            .type = InputType::Text,
            .text = std::move(text),
            .command = {},
            .attachments = std::move(attachments),
            .image_paths = std::move(images),
        };
    }

   private:
    auto trim(const std::string& s) -> std::string {
        auto start = s.find_first_not_of(" \t\n\r");
        auto end = s.find_last_not_of(" \t\n\r");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    }

    /// @brief 解析斜杠命令（如 /help, /tool (MCP) args）
    /// @param input 以 / 开头的输入字符串
    /// @return 解析后的命令，若无效返回 nullopt
    auto parse_slash_command(const std::string& input) -> std::optional<ParsedSlashCommand> {
        std::string without_slash = input.substr(1);
        std::vector<std::string> words = split(without_slash, ' ');

        if (words.empty()) {
            return std::nullopt;
        }

        ParsedSlashCommand result;
        result.command_name = words[0];
        size_t args_start = 1;

        // 处理 MCP 命令格式：/mcp:tool (MCP) arg1 arg2
        if (words.size() > 1 && words[1] == "(MCP)") {
            result.command_name += " (MCP)";
            result.is_mcp = true;
            args_start = 2;
        }

        // 拼接参数
        for (size_t i = args_start; i < words.size(); ++i) {
            if (i > args_start) result.args += " ";
            result.args += words[i];
        }

        return result;
    }

    auto split(const std::string& s, char delimiter) -> std::vector<std::string> {
        std::vector<std::string> tokens;
        std::string token;
        bool in_quote = false;

        for (char c : s) {
            if (c == '"') {
                in_quote = !in_quote;
                continue;
            }
            if (c == delimiter && !in_quote) {
                if (!token.empty()) {
                    tokens.push_back(std::move(token));
                    token.clear();
                }
            } else {
                token += c;
            }
        }

        if (!token.empty()) {
            tokens.push_back(std::move(token));
        }

        return tokens;
    }

    static auto is_image_ext(const std::string& path) -> bool {
        auto dot = path.rfind('.');
        if (dot == std::string::npos) return false;
        std::string ext = path.substr(dot);
        for (auto& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".bmp" ||
               ext == ".webp";
    }

    static const std::regex& ref_pattern() {
        // 匹配顺序: @"..." > @<...> > @\S+
        // 要求 @ 在词首（行首或前面是空白），避免误配 你好@world
        // 捕获组: 1=@"..."内容, 2=@<...>内容, 3=@word
        static const std::regex re(R"re((?:^|\s)@(?:"([^"]*)"|<([^>]+)>|(\S+)))re");
        return re;
    }

    auto extract_attachments(const std::string& text)
        -> std::tuple<std::string, std::vector<std::string>, std::vector<std::string>> {
        std::vector<std::string> attachments;
        std::vector<std::string> images;
        std::string cleaned_text;
        cleaned_text.reserve(text.size());

        auto begin = std::cregex_iterator(text.data(), text.data() + text.size(), ref_pattern());
        auto end = std::cregex_iterator();
        size_t last = 0;

        for (auto it = begin; it != end; ++it) {
            const auto& match = *it;

            // 添加匹配位置之前的文本
            cleaned_text.append(text.data() + last, match.position() - last);

            // 提取引用路径（三个捕获组互斥，取匹配的那个）
            // 原组1 (?:^|\s) 是非捕获组，@"..."=组1, @<...>=组2, @word=组3
            std::string ref;
            if (match[1].matched)
                ref = match[1].str();  // @"..."
            else if (match[2].matched)
                ref = match[2].str();  // @<...>
            else if (match[3].matched)
                ref = match[3].str();  // @word

            if (!ref.empty()) {
                if (is_image_ext(ref)) {
                    images.push_back(std::move(ref));
                } else {
                    attachments.push_back(std::move(ref));
                }
            }

            last = match.position() + match.length();
        }

        // 添加剩余文本
        cleaned_text.append(text.data() + last, text.size() - last);

        return {std::move(cleaned_text), std::move(attachments), std::move(images)};
    }
};

}  // namespace agent::input
