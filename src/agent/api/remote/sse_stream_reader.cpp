/**
 * @file sse_stream_reader.cpp
 * @brief SSE 流读取器实现
 * @details 解析 SSE/NDJSON 格式的流式响应，通过回调委托 Provider 特定解析
 * @version 2.0.0
 * @date 2026-07
 */

#include "agent/api/remote/sse_stream_reader.h"

#include <iostream>

#include "liblogger/logger.h"

namespace agent {

SSEStreamReader::SSEStreamReader(ParseSSECallback parse_cb)
    : m_parse_cb(std::move(parse_cb)),
      m_sse_parser([this](const SSEEvent& event) { on_sse_event(event); }) {}

SSEStreamReader::~SSEStreamReader() { cancel(); }

StreamState SSEStreamReader::next(std::function<bool()> should_stop, StreamChunk& out) {
    while (true) {
        // 检查取消
        if (m_cancelled.load()) {
            return StreamState::Cancelled;
        }

        // 检查外部停止
        if (should_stop && should_stop()) {
            cancel();
            return StreamState::Cancelled;
        }

        // 尝试从队列取数据
        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            if (!m_chunk_queue.empty()) {
                out = std::move(m_chunk_queue.front());
                m_chunk_queue.pop();
                if (out.is_final) {
                    return StreamState::Complete;
                }
                return StreamState::HasData;
            }
        }

        // 队列空，检查是否已经结束
        if (m_finished.load()) {
            if (!m_finish_error.empty()) {
                return StreamState::Error;
            }
            // 正常结束但没有 final chunk，构造一个
            out = StreamChunk{};
            out.is_final = true;
            out.content_delta = "";
            out.prompt_ms = 0.0;
            out.generation_ms = 0.0;
            return StreamState::Complete;
        }

        // 等待数据
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return !m_chunk_queue.empty() || m_finished.load() || m_cancelled.load();
            });
        }
    }
}

void SSEStreamReader::cancel() {
    if (m_cancelled.exchange(true)) return;  // 已取消则不重复日志
    LOG_DEBUG("[sse] reader cancelled, tokens_received={}", m_token_count);
    m_queue_cv.notify_all();
}

void SSEStreamReader::feed_data(std::string_view data) {
    if (m_cancelled.load() || m_finished.load()) return;
    m_sse_parser.parse(data);
}

void SSEStreamReader::finish(const std::string& error) { finish(0, error); }

void SSEStreamReader::finish(long http_code, const std::string& error) {
    if (m_finished.exchange(true)) return;  // 已 finish 则不重复
    m_finish_error = error;
    m_finish_status.store(static_cast<int>(http_code));
    if (error.empty()) {
        LOG_DEBUG("[sse] reader finished, tokens_received={}", m_token_count);
    } else {
        LOG_WARN("[sse] reader finished with error: http_status={} error={} tokens_received={}",
                 http_code, error, m_token_count);
    }
    m_queue_cv.notify_all();
}

int SSEStreamReader::http_status() const { return m_finish_status.load(); }

std::string SSEStreamReader::error_message() const { return m_finish_error; }

void SSEStreamReader::on_sse_event(const SSEEvent& event) {
    if (!event.has_data()) return;

    // 委托 Provider 特定解析回调
    StreamChunk chunk;
    if (m_parse_cb && m_parse_cb(event.event, event.data, chunk)) {
        // C.8：删除 m_content_buffer / m_reasoning_buffer 累积
        // 原实现累积所有 delta 但从未被读取，长响应下无限增长占内存
        m_token_count += chunk.token_count;

        {
            std::lock_guard<std::mutex> lock(m_queue_mutex);
            m_chunk_queue.push(std::move(chunk));
            LOG_INFO(
                "[sse] chunk queued, content_len={}, reasoning_len={}, "
                "tool_start={}, tool_delta={}, is_final={}, queue={}",
                chunk.content_delta.size(), chunk.reasoning_delta.size(), chunk.is_tool_use_start,
                chunk.is_tool_use_delta, chunk.is_final, m_chunk_queue.size());
        }
        m_queue_cv.notify_one();
    }
}

}  // namespace agent
