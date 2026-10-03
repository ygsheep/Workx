/**
 * @file permission_ask.cpp
 * @brief 权限确认工具实现
 * @details 发布 AskUserRequestEvent（带 result_promise），等待宿主填回
 *          AskUserResult；答案中任一项为 "Yes"（不区分大小写）即放行。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/permission_ask.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <vector>

#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"

namespace agent::tool {

namespace {

bool is_yes(const std::string& value) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "yes";
}

/// @brief 检测 Windows 磁盘格式化命令（`format C:` / `format /FS:...`）
/// @details 评审 #6：移除了宽泛的 `"format "` 子串匹配（误伤 printf("format %d")
///          等普通文本）。此处要求 `format` 为独立单词且后跟盘符（X:）或 `/` 参数。
bool is_disk_format_command(const std::string& s) noexcept {
    size_t pos = 0;
    while ((pos = s.find("format", pos)) != std::string::npos) {
        const bool left_ok = (pos == 0) || !std::isalnum(static_cast<unsigned char>(s[pos - 1]));
        const size_t after = pos + 6;
        if (left_ok && after < s.size() && (s[after] == ' ' || s[after] == '\t')) {
            const size_t p = s.find_first_not_of(" \t", after);
            if (p != std::string::npos) {
                if (s[p] == '/') return true;  // format /FS:... /Q
                if (p + 1 < s.size() && s[p + 1] == ':' &&
                    std::isalpha(static_cast<unsigned char>(s[p]))) {
                    return true;  // format C:
                }
            }
        }
        pos = after;
    }
    return false;
}

}  // namespace

bool ask_user_confirm(const ToolContext& ctx, const std::string& question, int timeout_ms) {
    if (!ctx.event_bus_ptr) return false;  // fail-closed：无确认通道即拒绝

    auto request = std::make_shared<AskUserRequestEvent>();
    auto wait_promise = std::make_shared<std::promise<AskUserResult>>();
    request->session_id = ctx.session_id;
    request->timeout_ms = timeout_ms;
    request->result_promise = wait_promise;
    request->cancel_flag = std::make_shared<std::atomic<bool>>(false);
    // 与 AskUserTool 契约一致：questions 字段为 {questions:[...]} 对象，
    // options 为 {label, description} 对象数组（ftxtui handle_ask_user 只解析
    // 对象选项；若发字符串数组会被当作无选项而静默取消，权限确认永不弹出）。
    request->questions = nlohmann::json::object(
        {{"questions", nlohmann::json::array({{
                           {"question", question},
                           {"header", "Permission Required"},
                           {"allow_custom_input", false},
                           {"options", nlohmann::json::array({
                                           {{"label", "Yes"}, {"description", "允许"}},
                                           {{"label", "No"}, {"description", "拒绝"}},
                                       })},
                       }})}});

    // 发布到总线；宿主（TUI）订阅 AskUserRequestEvent 后弹出确认面板
    // 注意：publish_async 模板以 T=AskUserRequestEvent 推导（typeid 匹配订阅），
    //       必须传值/解引用，不能传 shared_ptr（typeid 会变成 shared_ptr 类型）
    ctx.event_bus_ptr->publish_async(*request);

    auto future = wait_promise->get_future();
    if (future.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        return false;  // 超时/宿主未响应：拒绝
    }
    const AskUserResult result = future.get();
    if (!result.submitted) return false;
    for (const auto& [question_key, answer] : result.answers) {
        (void)question_key;
        if (is_yes(answer)) return true;
    }
    return false;
}

bool is_plan_mode(PermissionMode mode) noexcept { return mode == PermissionMode::Plan; }

bool is_bypass_mode(PermissionMode mode) noexcept {
    return mode == PermissionMode::BypassPermissions;
}

bool is_strict_mode(PermissionMode mode) noexcept { return mode == PermissionMode::Strict; }

bool deny_write_by_mode(PermissionMode mode) noexcept { return is_plan_mode(mode); }

bool deny_execute_by_mode(PermissionMode mode) noexcept { return is_plan_mode(mode); }

bool is_dangerous_command(const std::string& command) noexcept {
    static const std::vector<std::string> kPatterns = {
        // 破坏性文件操作
        "rm -rf",
        "rm -fr",
        "rm -r /",
        "rm -f /",
        "rmdir /s",
        "del /s",
        "del /f",
        "rd /s",
        "chmod 777",
        "chmod -r 777",
        "chown -r",
        // 磁盘/系统级
        "mkfs",
        "fdisk",
        "dd if=",
        "shutdown",
        "reboot",
        "halt",
        "poweroff",
        // 管道到 shell（远程执行模式）
        "| bash",
        "| sh ",
        "| zsh",
        "| powershell",
    };
    std::string lower = command;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& pattern : kPatterns) {
        if (lower.find(pattern) != std::string::npos) return true;
    }
    if (is_disk_format_command(lower)) return true;
    return false;
}

}  // namespace agent::tool