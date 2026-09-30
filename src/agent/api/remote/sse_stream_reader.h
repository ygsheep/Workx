/**
 * @file sse_stream_reader.h
 * @brief SSE 流读取器
 * @details 将 HTTP 响应数据通过 SSEParser 解析为 StreamChunk。
 *          通过 ParseSSECallback 委托 Provider 特定解析逻辑。
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include "agent/api/i_stream_reader.h"
#include "agent/api/sse_parser.hpp"

namespace agent {

/// @brief SSE 事件解析回调
/// @param event_type SSE event 类型（如 "content_block_delta"）
/// @param data SSE data 内容
/// @param out 输出的 StreamChunk
/// @return true 如果解析出了有效 chunk
using ParseSSECallback =
    std::function<bool(const std::string& event_type, const std::string& data, StreamChunk& out)>;

/// @brief SSE 流读取器
/// @details 从 HTTP 响应接收原始数据，通过 SSEParser 解析，通过回调输出 StreamChunk
class SSEStreamReader : public IStreamReader {
   public:
    /// @brief 构造
    /// @param parse_cb SSE 事件解析回调（Provider 特定）
    explicit SSEStreamReader(ParseSSECallback parse_cb);

    ~SSEStreamReader() override;

    // IStreamReader 接口
    StreamState next(std::function<bool()> should_stop, StreamChunk& out) override;
    void cancel() override;

    /// @brief 喂入原始 HTTP 数据（由 curl 回调调用）
    /// @param data 原始 SSE 数据块（C.10：改 string_view 避免 std::string 拷贝）
    void feed_data(std::string_view data);

    /// @brief 标记 HTTP 响应结束
    /// @param error 错误信息，空表示正常结束
    void finish(const std::string& error = "");

    /// @brief #89：带 HTTP 状态码标记响应结束
    /// @param http_code HTTP 状态码（0 表示无 HTTP 响应，如 curl 传输失败）
    /// @param error 错误信息，空表示正常结束
    /// @details 状态码此前被字符串化进 error 后丢弃，上层只能靠解析文本反推。
    ///          现在单独承载，供重试与降级判定结构化读取。
    void finish(long http_code, const std::string& error);

    /// @brief 是否已结束（包括正常结束和错误）
    bool is_finished() const { return m_finished.load(); }

    // IStreamReader 错误通道（#89）
    int http_status() const override;
    std::string error_message() const override;

   private:
    void on_sse_event(const SSEEvent& event);

    ParseSSECallback m_parse_cb;  ///< Provider 特定解析回调

    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::queue<StreamChunk> m_chunk_queue;

    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_finished{false};
    std::string m_finish_error;
    std::atomic<int> m_finish_status{0};  ///< #89：终止时的 HTTP 状态码（0 = 无 HTTP 响应）

    SSEParser m_sse_parser;

    // C.8：删除 m_content_buffer / m_reasoning_buffer
    // 原实现累积所有 delta 但从未被读取，长响应下无限增长占内存
    // （chat_renderer.h 有独立的 m_reasoning_buffer 用于 ctrl+o 视图，与本类无关）
    int32_t m_token_count = 0;
};

}  // namespace agent
