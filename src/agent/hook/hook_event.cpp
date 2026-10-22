/**
 * @file hook_event.h -> hook_event.cpp
 * @brief 事件枚举字符串转换 + HookDefinition::from_json
 * @version 1.0.1
 * @date 2026-08
 */

#include "agent/hook/hook_event.h"

#include <algorithm>
#include <utility>

namespace agent::hook {

const char* to_string(HookEvent event) noexcept {
    switch (event) {
        case HookEvent::PreToolUse:
            return "PreToolUse";
        case HookEvent::PostToolUse:
            return "PostToolUse";
        case HookEvent::SessionStart:
            return "SessionStart";
        case HookEvent::SessionEnd:
            return "SessionEnd";
        case HookEvent::Stop:
            return "Stop";
        case HookEvent::SubagentStart:
            return "SubagentStart";
        case HookEvent::SubagentStop:
            return "SubagentStop";
        case HookEvent::PermissionRequest:
            return "PermissionRequest";
    }
    return "Unknown";
}

std::optional<HookEvent> parse_event(const std::string& name) noexcept {
    const std::string lower = [&] {
        std::string s = name;
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }();
    if (lower == "pretooluse") return HookEvent::PreToolUse;
    if (lower == "posttooluse") return HookEvent::PostToolUse;
    if (lower == "sessionstart") return HookEvent::SessionStart;
    if (lower == "sessionend") return HookEvent::SessionEnd;
    if (lower == "stop") return HookEvent::Stop;
    if (lower == "subagentstart") return HookEvent::SubagentStart;
    if (lower == "subagentstop") return HookEvent::SubagentStop;
    if (lower == "permissionrequest") return HookEvent::PermissionRequest;
    return std::nullopt;
}

const char* type_to_string(HookType type) noexcept {
    switch (type) {
        case HookType::Command:
            return "command";
        case HookType::Prompt:
            return "prompt";
        case HookType::Agent:
            return "agent";
        case HookType::Http:
            return "http";
    }
    return "command";
}

namespace {
/// @brief 从 JSON 取值（字符串），缺失返回默认
std::string get_str(const nlohmann::json& obj, const char* key, const std::string& fallback = {}) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

int get_int(const nlohmann::json& obj, const char* key, int fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return fallback;
    return it->get<int>();
}

bool get_bool(const nlohmann::json& obj, const char* key, bool fallback) {
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

std::vector<std::string> get_str_vec(const nlohmann::json& obj, const char* key) {
    std::vector<std::string> out;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}
}  // anonymous namespace

HookDefinition HookDefinition::from_json(const nlohmann::json& obj) {
    HookDefinition def;

    const std::string event_name = get_str(obj, "event");
    if (auto ev = parse_event(event_name); ev)
        def.event = *ev;
    else if (!event_name.empty())
        def.event = HookEvent::PreToolUse;  // 未知事件，安全默认

    const std::string type_name = get_str(obj, "type", "command");
    if (type_name == "prompt")
        def.type = HookType::Prompt;
    else if (type_name == "agent")
        def.type = HookType::Agent;
    else if (type_name == "http")
        def.type = HookType::Http;
    else
        def.type = HookType::Command;

    def.match = get_str(obj, "match");
    def.command = get_str(obj, "command");
    def.url = get_str(obj, "url");
    if (obj.contains("headers") && obj["headers"].is_object()) {
        def.headers = obj["headers"];
    }
    def.allowedEnvVars = get_str_vec(obj, "allowedEnvVars");
    def.prompt = get_str(obj, "prompt");
    def.model = get_str(obj, "model");
    def.agent = get_str(obj, "agent");
    def.timeout_ms = get_int(obj, "timeout_ms", 30000);
    def.statusMessage = get_bool(obj, "statusMessage", false);
    def.once = get_bool(obj, "once", false);
    def.async = get_bool(obj, "async", false);
    def.asyncRewake = get_bool(obj, "asyncRewake", false);
    return def;
}

}  // namespace agent::hook
