/**
 * @file secret_scanner.h
 * @brief 密钥扫描器
 * @details 检测文本中的潜在密钥（API key、token、私钥等）。
 *          规则集移植自 Claude Code secretScanner.ts（gitleaks 高置信度子集）。
 *          v1.0.0：10 条规则覆盖主流云/AI/VCS/通讯/支付服务商。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agent::tool {

/// @brief 密钥匹配结果
struct SecretMatch {
    std::string rule_id;  ///< 规则 ID，如 "github-pat"
    std::string label;    ///< 人类可读标签，如 "GitHub PAT"
};

/// @brief 扫描文本中的密钥
/// @details 使用预编译的正则规则集（懒加载，首次调用编译并缓存）。
///          规则按 rule_id 去重（同规则多次命中只返回一次）。
///          匹配文本不返回（避免日志/显示泄露密钥本身）。
/// @param content 待扫描文本
/// @return 命中的规则列表；无命中返回空
std::vector<SecretMatch> scan_for_secrets(const std::string& content);

/// @brief 扫描并构造错误信息
/// @details 便捷封装：扫描 + 拼接错误信息。
///          无命中返回空字符串；有命中返回：
///          "Content contains potential secrets (label1, label2) and cannot be written to file. "
///          "Remove the sensitive content and try again."
/// @param content 待扫描文本
/// @return 无命中返回空字符串，否则返回错误信息
std::string scan_for_secret_error(const std::string& content);

/// @brief 脱敏文本中的密钥（#36）
/// @details 将规则命中的密钥内容替换为 "[REDACTED:label]"，
///          供命令输出/文件读取展示时遮蔽敏感内容。无命中则原样返回。
/// @param content 待脱敏文本
/// @return 脱敏后文本
std::string redact_secrets(const std::string& content);

}  // namespace agent::tool
