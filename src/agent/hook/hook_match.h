/**
 * @file hook_match.h
 * @brief Hook `if` 条件匹配器 — permission-rule 语法一次性编译
 * @details 对齐 cc preparePermissionMatcher()：为每个 hook 的 match 字段
 *          一次编译成 matcher，dispatch 时 O(1) 判断命中，避免每次重新解析。
 *          语法：`ToolName(arg pattern)`，多个以 `||` 连接；
 *          空 match = 匹配所有（全命中）。
 * @version 1.0.1
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace agent::hook {

/// @brief hook `if` 条件匹配器（一个 def.match 编译产物）
/// @details match 格式（permission-rule，对齐 cc）：
///          - 空字符串 → 匹配所有
///          - `ToolName` → 按工具名匹配（无参数约束）
///          - `ToolName(args)` → 工具名 + 参数 glob 匹配
///          - 多个子句以 ` || ` 分隔 → 任一命中即通过
///          - `*` 通配（glob，含 `*` 前缀/后缀）
class HookMatcher {
   public:
    /// @brief 编译 match 表达式（空 → 全命中）
    explicit HookMatcher(std::string expr);

    /// @brief 是否命中当前事件（事件名 + 工具名 + 工具输入）
    /// @details event_name：hook 触发时的事件字符串（"PreToolUse" 等）；
    ///          tool_name / tool_input 仅 PreToolUse/PostToolUse/PermissionRequest 有值。
    bool matches(const std::string& event_name, const std::string& tool_name,
                 const nlohmann::json& tool_input) const noexcept;

    /// @brief 是否为"全命中"占位（空 match）
    bool matches_all() const noexcept { return all_; }

   private:
    struct Rule {
        std::string event;  // 事件名，空 = 不限
        std::string tool;   // 工具名，含 * 通配
        std::string arg;    // 参数 glob，空 = 不限
    };
    std::vector<Rule> rules_;
    bool all_ = false;  // 空表达式全命中

    static bool glob_match(const std::string& pattern, const std::string& text) noexcept;
    void parse_rules(const std::string& expr);
};

}  // namespace agent::hook
