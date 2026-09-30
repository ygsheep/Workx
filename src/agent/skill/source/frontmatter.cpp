/**
 * @file frontmatter.cpp
 * @brief Skill 文件 frontmatter 解析实现
 * @details 极简 key: value 解析器：按行解析，不支持嵌套 YAML。
 *          当前字段均为扁平标量，够用即可；未来需要嵌套结构再引入 yaml-cpp。
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/skill/inclaude/frontmatter.h"

#include <cctype>
#include <string_view>

#include <nlohmann/json.hpp>

namespace agent::skill {

namespace {

/// @brief 去首尾空白（含 \r，兼容 CRLF 文件）
std::string trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

/// @brief 是否为 `---` 分隔行
bool is_fence(std::string_view line) { return trim(line) == "---"; }

/// @brief 解析布尔值；无法识别时返回 fallback
bool parse_bool(std::string_view value, bool fallback) {
    const auto v = trim(value);
    if (v == "true" || v == "yes" || v == "1") return true;
    if (v == "false" || v == "no" || v == "0") return false;
    return fallback;
}

/// @brief 剥离成对引号（YAML 字符串字面量）
std::string strip_quotes(std::string_view v) {
    if (v.size() >= 2 &&
        ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\''))) {
        return std::string(v.substr(1, v.size() - 2));
    }
    return std::string(v);
}

/// @brief 解析 aliases：支持 `a, b` 与 `[a, b]` 形式，逐项剥离 YAML 引号
std::vector<std::string> parse_aliases(std::string_view value) {
    auto v = trim(value);
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
        v = v.substr(1, v.size() - 2);
    }
    std::vector<std::string> aliases;
    size_t pos = 0;
    while (pos <= v.size()) {
        const size_t comma = v.find(',', pos);
        const auto item =
            trim(v.substr(pos, comma == std::string_view::npos ? v.size() - pos : comma - pos));
        if (!item.empty()) aliases.push_back(strip_quotes(item));
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return aliases;
}

/// @brief 解析 paths：同 aliases 的列表形式（引号剥离已在 parse_aliases 内完成）
std::vector<std::string> parse_paths(std::string_view value) { return parse_aliases(value); }

/// @brief 从正文首行派生描述（去掉 # 前缀与空白）
std::string derive_description(const std::string& body) {
    const size_t first_newline = body.find('\n');
    auto first_line = trim(body.substr(0, first_newline));
    if (first_line.size() >= 2 && first_line[0] == '#') {
        first_line = trim(first_line.substr(1));
    }
    return first_line;
}

/// @brief 将 frontmatter 中对象式 hook 的「原始对象块」转换为 JSON 数组字符串
/// @details 每个 block 为一块多行文本（每行 `key: value`，键已小写、值已去引号），
///          仅支持扁平对象（键值对逐行），兼容字段值内含逗号/括号等（取首个冒号分割）。
///          标量推断：true/false → 布尔；纯数字 → 整数；其余 → 字符串。
std::string yaml_objects_to_json(const std::vector<std::string>& blocks) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& block : blocks) {
        nlohmann::json obj = nlohmann::json::object();
        size_t pos = 0;
        while (pos <= block.size()) {
            const size_t nl = block.find('\n', pos);
            const auto raw =
                block.substr(pos, nl == std::string::npos ? block.size() - pos : nl - pos);
            pos = nl == std::string::npos ? block.size() + 1 : nl + 1;
            const auto line = trim(raw);
            if (line.empty() || line.front() == '#') continue;
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;  // 缺冒号（如子列表项）忽略
            std::string key = trim(line.substr(0, colon));
            if (key.empty()) continue;
            std::string value = strip_quotes(trim(line.substr(colon + 1)));
            if (value == "true") {
                obj[key] = true;
            } else if (value == "false") {
                obj[key] = false;
            } else {
                bool is_int = !value.empty();
                for (char c : value) {
                    if (c < '0' || c > '9') {
                        is_int = false;
                        break;
                    }
                }
                obj[key] =
                    is_int ? nlohmann::json(std::stol(value)) : nlohmann::json(std::move(value));
            }
        }
        arr.push_back(std::move(obj));
    }
    return arr.dump();
}

}  // anonymous namespace

ParsedSkill parse_skill_content(const std::string& content, const std::string& default_name) {
    ParsedSkill result;
    result.frontmatter.name = default_name;

    // 无 frontmatter：全部为正文
    if (content.empty() ||
        !is_fence(std::string_view(content).substr(
            0, content.find('\n') == std::string::npos ? content.size() : content.find('\n')))) {
        result.body = content;
        result.frontmatter.description = derive_description(result.body);
        return result;
    }

    // 分离 frontmatter 块与正文
    std::string_view front_part;
    std::string_view body_part;
    {
        std::string_view rest = content;
        // 跳过第一行（---）
        const size_t first_nl = rest.find('\n');
        rest = first_nl == std::string_view::npos ? std::string_view{} : rest.substr(first_nl + 1);

        // 找第二个 --- 行
        const size_t second_fence = rest.find("\n---");
        if (second_fence == std::string_view::npos) {
            // 未闭合：整个文档视为正文，无 frontmatter 字段
            result.body = content;
            result.frontmatter.description = derive_description(result.body);
            return result;
        }
        front_part = rest.substr(0, second_fence);
        size_t body_start = second_fence + 4;                                    // 跳过 "\n---"
        if (body_start < rest.size() && rest[body_start] == '\r') ++body_start;  // CRLF
        if (body_start < rest.size() && rest[body_start] == '\n') ++body_start;
        body_part = rest.substr(body_start);
    }

    result.body = std::string(body_part);

    // 逐行解析 key: value
    size_t pos = 0;
    std::string last_list_key;   // 最近一个列表 key（支持多行 `- item` 追加）
    bool obj_hook_open = false;  // 当前是否在收集一个对象式 hook
    std::vector<std::string> obj_blocks;  // 已完成的原始对象块（每块为多行 `key: value`）
    std::string cur_obj_block;            // 正在收集的原始对象块
    while (pos <= front_part.size()) {
        const size_t nl = front_part.find('\n', pos);
        const auto line = front_part.substr(
            pos, nl == std::string_view::npos ? front_part.size() - pos : nl - pos);
        pos = nl == std::string_view::npos ? front_part.size() + 1 : nl + 1;

        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;

        size_t indent = 0;
        while (indent < line.size() && (line[indent] == ' ' || line[indent] == '\t')) ++indent;
        const bool starts_dash = trimmed.front() == '-';
        const std::string item_text = starts_dash ? trim(trimmed.substr(1)) : std::string(trimmed);
        const bool content_has_colon = item_text.find(':') != std::string::npos;

        // ---- 对象式 hooks 状态机（`hooks:` 下的 `- event: ...` 列表） ----
        if (last_list_key == "hooks") {
            if (starts_dash) {
                if (content_has_colon) {
                    // 新对象项：flush 上一个对象块
                    if (obj_hook_open && !cur_obj_block.empty()) {
                        obj_blocks.push_back(cur_obj_block);
                        cur_obj_block.clear();
                    }
                    obj_hook_open = true;
                    cur_obj_block = item_text;
                } else {
                    // 传统 command hook（非对象）：结束对象模式
                    if (obj_hook_open && !cur_obj_block.empty()) {
                        obj_blocks.push_back(cur_obj_block);
                        cur_obj_block.clear();
                        obj_hook_open = false;
                    }
                    if (!item_text.empty())
                        result.frontmatter.hooks.push_back(strip_quotes(item_text));
                }
                continue;
            }
            if (obj_hook_open && indent > 0) {
                // 对象续行（子键 `type: command` 等）：追加到当前对象块
                cur_obj_block += "\n" + std::string(trimmed);
                continue;
            }
            // 缩进为 0 的新字段：结束 hooks 块，回落做普通 key 解析
            last_list_key.clear();
            if (obj_hook_open && !cur_obj_block.empty()) {
                obj_blocks.push_back(cur_obj_block);
                cur_obj_block.clear();
                obj_hook_open = false;
            }
        }

        // 多行列表项：`- item` 追加到上一个列表 key（aliases / paths）
        if (!last_list_key.empty() && starts_dash) {
            if (!item_text.empty()) {
                if (last_list_key == "paths")
                    result.frontmatter.paths.push_back(strip_quotes(item_text));
                else if (last_list_key == "aliases")
                    result.frontmatter.aliases.push_back(strip_quotes(item_text));
            }
            continue;
        }

        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos) {
            last_list_key.clear();
            continue;
        }

        std::string key = trimmed.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key = trim(key);
        const std::string value = trim(trimmed.substr(colon + 1));

        if (key == "name") {
            if (!value.empty()) result.frontmatter.name = value;
            last_list_key.clear();
        } else if (key == "description") {
            result.frontmatter.description = value;
            last_list_key.clear();
        } else if (key == "aliases") {
            result.frontmatter.aliases = parse_aliases(value);
            last_list_key = "aliases";
        } else if (key == "argument_hint") {
            result.frontmatter.argument_hint = value;
            last_list_key.clear();
        } else if (key == "when_to_use") {
            result.frontmatter.when_to_use = value;
            last_list_key.clear();
        } else if (key == "context") {
            result.frontmatter.context = value;
            last_list_key.clear();
        } else if (key == "agent") {
            result.frontmatter.agent = value;
            last_list_key.clear();
        } else if (key == "hooks") {
            result.frontmatter.hooks = parse_paths(value);
            last_list_key = "hooks";
        } else if (key == "model") {
            result.frontmatter.model = value;
            last_list_key.clear();
        } else if (key == "user_invocable") {
            result.frontmatter.user_invocable = parse_bool(value, true);
            last_list_key.clear();
        } else if (key == "disable_model_invocation") {
            result.frontmatter.disable_model_invocation = parse_bool(value, false);
            last_list_key.clear();
        } else if (key == "paths") {
            result.frontmatter.paths = parse_paths(value);
            last_list_key = "paths";
        } else {
            // 未知字段忽略（扩展点）
            last_list_key.clear();
        }
    }

    // 收尾：flush 最后一个对象块并序列化为 JSON
    if (obj_hook_open && !cur_obj_block.empty()) {
        obj_blocks.push_back(cur_obj_block);
    }
    if (!obj_blocks.empty()) {
        result.frontmatter.hooks_json.push_back(yaml_objects_to_json(obj_blocks));
    }

    if (result.frontmatter.description.empty()) {
        result.frontmatter.description = derive_description(result.body);
    }
    return result;
}

}  // namespace agent::skill
