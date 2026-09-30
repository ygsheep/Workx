/**
 * @file jsonl_protocol.cpp
 * @brief Island IPC JSONL 协议编解码实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "island/jsonl_protocol.h"

#include <chrono>

namespace island {

std::string serialize_event(const std::string& type, const nlohmann::json& data, int64_t seq,
                            double ts) {
    nlohmann::json j{
        {"kind", "event"}, {"type", type}, {"seq", seq}, {"ts", ts}, {"data", data},
    };
    return j.dump() + "\n";
}

std::string serialize_request(const std::string& type, const nlohmann::json& data,
                              const std::string& id) {
    nlohmann::json j{
        {"kind", "request"},
        {"type", type},
        {"id", id},
        {"data", data},
    };
    return j.dump() + "\n";
}

std::string serialize_response(const std::string& id, bool ok, const nlohmann::json& data) {
    nlohmann::json j{
        {"kind", "response"},
        {"id", id},
        {"ok", ok},
        {"data", data},
    };
    return j.dump() + "\n";
}

std::optional<Envelope> parse_line(std::string_view line) {
    if (line.empty()) return std::nullopt;

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    if (!j.is_object()) return std::nullopt;

    const std::string kind = j.value("kind", "");
    Envelope env;
    if (kind == "event") {
        env.kind = MsgKind::Event;
    } else if (kind == "request") {
        env.kind = MsgKind::Request;
    } else if (kind == "response") {
        env.kind = MsgKind::Response;
    } else {
        return std::nullopt;
    }

    env.type = j.value("type", "");
    env.seq = j.value("seq", static_cast<int64_t>(0));
    env.ts = j.value("ts", 0.0);
    env.id = j.value("id", "");
    env.ok = j.value("ok", true);
    env.data = j.value("data", nlohmann::json::object());
    return env;
}

double now_ts() {
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const double sec = duration_cast<duration<double>>(now).count();
    return static_cast<double>(static_cast<int64_t>(sec * 1000.0)) / 1000.0;
}

}  // namespace island