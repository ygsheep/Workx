/**
 * @file secret_scanner.cpp
 * @brief 密钥扫描器实现
 * @details 规则集移植自 Claude Code secretScanner.ts。
 *          使用 std::regex（ECMAScript 语法），首次调用懒加载并缓存到函数静态变量。
 *          性能权衡：std::regex 较慢但零依赖；MVP 阶段足够，未来可换 re2。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/secret_scanner.h"

#include <algorithm>
#include <format>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace agent::tool {

namespace {

/// @brief 密钥规则
struct SecretRule {
    std::string id;
    std::string label;
    std::regex re;
};

/// @brief kebab-case 规则 id → 人类可读标签
/// @details 直接查硬编码映射表（10+ 条规则有限且稳定），未命中时回退返回原 id。
///          评审意见：原逐字符拼接循环基本无效（死代码），已移除。
std::string rule_id_to_label(const std::string& id) {
    static const std::unordered_map<std::string, std::string> id_to_label = {
        {"aws-access-token", "AWS Access Token"},
        {"gcp-api-key", "GCP API Key"},
        {"github-pat", "GitHub PAT"},
        {"github-fine-grained-pat", "GitHub Fine-Grained PAT"},
        {"github-oauth", "GitHub OAuth"},
        {"gitlab-pat", "GitLab PAT"},
        {"slack-bot-token", "Slack Bot Token"},
        {"slack-user-token", "Slack User Token"},
        {"anthropic-api-key", "Anthropic API Key"},
        {"openai-api-key", "OpenAI API Key"},
        {"private-key", "Private Key"},
        {"stripe-access-token", "Stripe Access Token"},
    };
    const auto it = id_to_label.find(id);
    return it == id_to_label.end() ? id : it->second;
}

/// @brief 构造规则集（懒加载）
const std::vector<SecretRule>& rules() {
    static const std::vector<SecretRule> kRules = []() {
        std::vector<SecretRule> v;
        // ECMAScript 语法（默认），与 JS RegExp 基本兼容
        constexpr auto kECMA = std::regex::ECMAScript;
        constexpr auto kECMA_ICASE = std::regex::ECMAScript | std::regex::icase;

        auto add = [&](std::string id, std::string pattern, bool icase = false) {
            SecretRule r;
            r.id = std::move(id);
            r.label = rule_id_to_label(r.id);
            r.re = std::regex(pattern, icase ? kECMA_ICASE : kECMA);
            v.push_back(std::move(r));
        };

        // === 云提供商 ===
        add("aws-access-token", R"(AKIA[0-9A-Z]{16})");
        add("aws-access-token", R"(ASIA[0-9A-Z]{16})");
        add("gcp-api-key", R"(AIza[0-9A-Za-z\-_]{35})");

        // === 版本控制 ===
        add("github-pat", R"(ghp_[0-9a-zA-Z]{36})");
        add("github-fine-grained-pat", R"(github_pat_[0-9a-zA-Z_]{82})");
        add("github-oauth", R"(gho_[0-9a-zA-Z]{36})");
        add("gitlab-pat", R"(glpat-[0-9a-zA-Z\-_]{20})");

        // === 通讯 ===
        add("slack-bot-token", R"(xoxb-[0-9]{10,}-[0-9]{10,}-[0-9a-zA-Z]{12,})");
        add("slack-user-token", R"(xox[pe]-[0-9]{10,}-[0-9]{10,}-[0-9a-zA-Z]{12,})");

        // === AI APIs ===
        add("anthropic-api-key", R"(sk-ant-api[0-9]{2}-[0-9a-zA-Z\-_]{20,})");
        add("openai-api-key", R"(sk-proj-[0-9a-zA-Z\-_]{20,})");
        add("openai-api-key", R"(sk-svcacct-[0-9a-zA-Z\-_]{20,})");
        add("openai-api-key", R"(sk-admin-[0-9a-zA-Z\-_]{20,})");

        // === 支付 ===
        add("stripe-access-token", R"((sk|rk)_(test|live|prod)_[0-9a-zA-Z]{10,})");

        // === 加密 ===
        add("private-key", R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)", true);

        return v;
    }();
    return kRules;
}

}  // anonymous namespace

std::vector<SecretMatch> scan_for_secrets(const std::string& content) {
    std::vector<SecretMatch> matches;
    std::unordered_set<std::string> seen;

    // 评审 #5（技术债）：对每次读写全量跑 13 条 std::regex（含 private-key 大模式），
    // 大文件/高频读写开销明显。MVP 可接受；后续建议换 re2，或在扫描前加
    // 长度/类型前置过滤（如仅对含 "token"/"key"/"-----BEGIN" 的片段扫描）。
    for (const auto& rule : rules()) {
        try {
            if (std::regex_search(content, rule.re)) {
                // 按 rule_id 去重
                if (seen.insert(rule.id).second) {
                    matches.push_back({rule.id, rule.label});
                }
            }
        } catch (const std::regex_error&) {
            // 规则编译/匹配异常时跳过（不阻断扫描）
        }
    }
    return matches;
}

std::string scan_for_secret_error(const std::string& content) {
    auto matches = scan_for_secrets(content);
    if (matches.empty()) return {};

    std::string labels;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) labels += ", ";
        labels += matches[i].label;
    }
    return std::format(
        "Content contains potential secrets ({}) and cannot be written to file. "
        "Remove the sensitive content and try again.",
        labels);
}

std::string redact_secrets(const std::string& content) {
    if (content.empty()) return content;
    std::string out(content);
    for (const auto& rule : rules()) {
        try {
            out = std::regex_replace(out, rule.re, "[REDACTED:" + rule.label + "]");
        } catch (const std::regex_error&) {
            // 规则异常时跳过（不阻断脱敏）
        }
    }
    return out;
}

}  // namespace agent::tool
