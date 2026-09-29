/**
 * @file hook_match.cpp
 * @brief HookMatcher — permission-rule 语法解析 + glob 匹配
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/hook/hook_match.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace agent::hook {

namespace {

/// @brief 字符串 trim（空格）
std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// @brief 单个 glob 通配匹配（`*` = 任意序列，其余字面）
bool glob_match_impl(const char* pat, const char* text) noexcept {
    const char* star_p = nullptr;
    const char* star_t = nullptr;
    while (*text) {
        if (*pat == '*') {
            while (*pat == '*') ++pat;   // 折叠连续 *
            if (*pat == 0) return true;  // 尾随 * 匹配剩余
            star_p = pat;
            star_t = text;
            continue;
        }
        if (*pat == '?' || *pat == *text) {
            ++pat;
            ++text;
            continue;
        }
        if (star_p) {  // 回溯到上一个 *
            pat = star_p;
            text = ++star_t;
            continue;
        }
        return false;
    }
    while (*pat == '*') ++pat;
    return *pat == 0;
}

/// @brief 大小写不敏感 glob（工具名/参数一般按大小写敏感匹配较合理，
///        但事件名做不敏感；此处保持字面匹配）
bool glob(const std::string& pattern, const std::string& text) noexcept {
    return glob_match_impl(pattern.c_str(), text.c_str());
}

/// @brief 解析单条子句：`Tool(args)` 或仅 `Tool`
/// @param clause 输入，如 `Bash(git *)`、`Read`、`Bash(rm -rf *)`
/// @return {tool, arg, ok}
struct ParsedClause {
    std::string tool;
    std::string arg;
    bool ok = false;
};
ParsedClause parse_clause(const std::string& raw) {
    ParsedClause out;
    const auto lp = raw.find('(');
    if (lp == std::string::npos) {
        out.tool = trim(raw);
        out.ok = !out.tool.empty();
        return out;
    }
    if (raw.back() != ')') return out;  // 不完整括号
    out.tool = trim(raw.substr(0, lp));
    out.arg = raw.substr(lp + 1, raw.size() - lp - 2);
    out.ok = !out.tool.empty();
    return out;
}

/// @brief 大小写不敏感的完整字符串比较（用于事件名匹配）
bool iequals(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/// @brief 工具输入扁平文本（展开所有字符串值，供 arg glob 做包含匹配）
std::vector<std::string> collect_strings(const nlohmann::json& input) {
    std::vector<std::string> out;
    if (input.is_string()) {
        out.push_back(input.get<std::string>());
        return out;
    }
    if (input.is_object()) {
        for (auto it = input.begin(); it != input.end(); ++it) {
            if (it.value().is_string()) {
                out.push_back(it.value().get<std::string>());
            } else if (it.value().is_object() || it.value().is_array()) {
                auto nested = collect_strings(it.value());
                out.insert(out.end(), nested.begin(), nested.end());
            }
        }
    } else if (input.is_array()) {
        for (const auto& v : input) {
            if (v.is_string()) {
                out.push_back(v.get<std::string>());
            } else if (v.is_object() || v.is_array()) {
                auto nested = collect_strings(v);
                out.insert(out.end(), nested.begin(), nested.end());
            }
        }
    }
    return out;
}

}  // anonymous namespace

void HookMatcher::parse_rules(const std::string& expr) {
    rules_.clear();
    all_ = false;
    const std::string e = trim(expr);
    if (e.empty()) {
        all_ = true;
        return;
    }

    // 以 ` || ` 分隔多个子句（cc permission-rule 风格）
    std::string clause;
    auto push_clause = [&] {
        if (clause.empty()) return;
        auto pc = parse_clause(clause);
        if (pc.ok) {
            Rule rule;
            rule.tool = pc.tool;
            rule.arg = pc.arg;
            rules_.push_back(std::move(rule));
        }
        clause.clear();
    };
    for (size_t i = 0; i < e.size(); ++i) {
        if (i + 3 < e.size() && e[i] == ' ' && e[i + 1] == '|' && e[i + 2] == '|' &&
            e[i + 3] == ' ') {
            push_clause();
            i += 3;
            continue;
        }
        clause.push_back(e[i]);
    }
    push_clause();
    if (rules_.empty()) all_ = true;  // 全部解析失败 → 保守全命中（不误拦）
}

HookMatcher::HookMatcher(std::string expr) : all_(false) { parse_rules(std::move(expr)); }

bool HookMatcher::matches(const std::string& event_name, const std::string& tool_name,
                          const nlohmann::json& tool_input) const noexcept {
    if (all_) return true;
    if (rules_.empty()) return false;

    for (const auto& rule : rules_) {
        // 事件名匹配（若指定）：子句是 `Event/Tool(...)` 时事件前缀，本实现不支持
        // 事件前缀，事件已由 dispatch 的 event 参数过滤，此处只做工具名匹配。
        (void)event_name;
        if (!glob(rule.tool, tool_name)) continue;
        if (rule.arg.empty()) return true;

        // 参数匹配：对工具输入展开的每个字符串值，做 glob 或字面包含匹配。
        // 不使用整串 dump（会引入 JSON 语法字符干扰 glob），逐个语义值更贴近 cc。
        for (const auto& v : collect_strings(tool_input)) {
            if (v.empty()) continue;
            if (glob(rule.arg, v)) return true;
            // glob 未命中且 arg 无通配时，退化为子串包含（如 "rm -rf" 无 *）
            if (rule.arg.find_first_of("*?") == std::string::npos &&
                v.find(rule.arg) != std::string::npos)
                return true;
        }
    }
    return false;
}

bool HookMatcher::glob_match(const std::string& pattern, const std::string& text) noexcept {
    return glob(pattern, text);
}

}  // namespace agent::hook
