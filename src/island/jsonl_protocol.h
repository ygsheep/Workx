/**
 * @file jsonl_protocol.h
 * @brief Island IPC 双通道 JSONL 协议编解码
 * @details 消息外层信封：
 *          {"kind":"event","type":..,"seq":..,"ts":..,"data":{...}}
 *          {"kind":"request","type":..,"id":..,"data":{...}}
 *          {"kind":"response","id":..,"ok":..,"data":{...}}
 *          - seq：TUI 单调递增事件序号（断线重连后从 last_seq 续传）
 *          - ts：Unix 时间戳（秒·毫秒，double）
 *          - id：请求 ID，响应以同 id 关联
 *          GUI 端尚未实现，本文件为 GUI 实现的协议契约。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace island {

/// @brief 信封 kind
enum class MsgKind {
    Event,     ///< 事件流推送（TUI → GUI）
    Request,   ///< 请求（GUI → TUI）
    Response,  ///< 响应（TUI → GUI，以 id 关联请求）
};

/// @brief 解码后的消息信封
struct Envelope {
    MsgKind kind = MsgKind::Event;
    std::string type;     ///< 事件/请求类型名
    int64_t seq = 0;      ///< 事件序号（仅 Event 有效）
    double ts = 0.0;      ///< Unix 时间戳（秒·毫秒）
    std::string id;       ///< 请求 ID（仅 Request/Response 有效）
    bool ok = true;       ///< 响应是否成功（仅 Response 有效）
    nlohmann::json data;  ///< 载荷
};

/// @brief 序列化事件消息（含换行，供直接 write）
std::string serialize_event(const std::string& type, const nlohmann::json& data, int64_t seq,
                            double ts);

/// @brief 序列化请求消息（含换行）
std::string serialize_request(const std::string& type, const nlohmann::json& data,
                              const std::string& id);

/// @brief 序列化响应消息（含换行）
std::string serialize_response(const std::string& id, bool ok, const nlohmann::json& data);

/// @brief 解析一行 JSONL；解析失败返回 nullopt（调用方丢弃该行/累计失败计数）
std::optional<Envelope> parse_line(std::string_view line);

/// @brief 当前 Unix 时间戳（秒·毫秒）
double now_ts();

}  // namespace island