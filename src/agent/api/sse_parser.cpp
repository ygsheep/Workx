/**
 * @file sse_parser.cpp
 * @brief SSE + NDJSON 流式解析器实现
 */

#include <liblogger/logger.h>
#include "agent/api/sse_parser.hpp"
#include <algorithm>

namespace agent {

SSEParser::SSEParser(EventCallback callback) : m_callback(std::move(callback)) {}

void SSEParser::parse(std::string_view chunk) {
    if (chunk.empty()) return;
    m_buffer.append(chunk);
    process_events();
}

void SSEParser::process_events() {
    if (m_buffer.empty()) return;

    size_t pos = 0;
    size_t last_event_end = 0;

    while (pos < m_buffer.length()) {
        size_t event_end = std::string::npos;
        size_t sep_len = 0;  // 实际匹配的分隔符长度

        // SSE 事件分隔符（按优先级匹配；三者互不包含）
        size_t nn_pos = m_buffer.find("\n\n", pos);
        if (nn_pos != std::string::npos) {
            event_end = nn_pos + 2;
            sep_len = 2;
        }

        if (event_end == std::string::npos) {
            size_t rn_pos = m_buffer.find("\r\n\r\n", pos);
            if (rn_pos != std::string::npos) {
                event_end = rn_pos + 4;
                sep_len = 4;
            }
        }

        if (event_end == std::string::npos) {
            size_t rr_pos = m_buffer.find("\r\r", pos);
            if (rr_pos != std::string::npos) {
                event_end = rr_pos + 2;
                sep_len = 2;
            }
        }

        if (event_end == std::string::npos) break;

        // 截取事件内容（按实际分隔符长度剥离，避免 \r\n\r\n 时末尾残留 \r\n）
        std::string event_text = m_buffer.substr(pos, event_end - pos - sep_len);

        // 兼容性：剥离末尾残留 '\r'（如混合行尾 "data: x\r\n\ndata" 残留的 '\r'）
        if (!event_text.empty() && event_text.back() == '\r') {
            event_text.pop_back();
        }

        if (!event_text.empty()) {
            SSEEvent event = parse_event(event_text);
            if (event.has_data()) {
                m_callback(event);
                m_event_count++;
            }
        }

        last_event_end = event_end;
        pos = event_end;
    }

    if (last_event_end > 0) {
        // C.9：substr → erase，避免重新分配内存
        // erase 仍 O(n) 但就地修改，不构造新 string 对象
        m_buffer.erase(0, last_event_end);
    }
}

SSEEvent SSEParser::parse_event(std::string_view event_text) {
    SSEEvent event;

    // 按 \n 拆分（避免 istringstream 拷贝开销）
    size_t pos = 0;
    while (pos < event_text.size()) {
        size_t nl = event_text.find('\n', pos);
        size_t line_end = (nl != std::string_view::npos) ? nl : event_text.size();
        std::string_view line_sv = event_text.substr(pos, line_end - pos);
        // 兼容 \r\n：剥离末尾 '\r'
        if (!line_sv.empty() && line_sv.back() == '\r') {
            line_sv.remove_suffix(1);
        }
        pos = (nl != std::string_view::npos) ? nl + 1 : event_text.size();

        if (line_sv.empty()) continue;

        size_t colon_pos = line_sv.find(':');
        if (colon_pos == std::string_view::npos) continue;

        std::string_view field = line_sv.substr(0, colon_pos);
        std::string_view value;

        if (colon_pos + 1 < line_sv.size()) {
            if (line_sv[colon_pos + 1] == ' ') {
                value = line_sv.substr(colon_pos + 2);
            } else {
                value = line_sv.substr(colon_pos + 1);
            }
        }

        if (field == "data") {
            if (event.has_data()) {
                event.data += "\n";
                event.data.append(value);
            } else {
                event.data.assign(value);
            }
        } else if (field == "event") {
            event.event.assign(value);
        } else if (field == "id") {
            event.id.assign(value);
        } else if (field == "retry") {
            try {
                event.retry = std::stoi(std::string(value));
            } catch (...) {
            }
        }
    }
    // 安全：不记录 event.data 内容，避免泄露响应正文
    LOG_INFO("SSEParser parsed event: data_len={}, has_event={}", event.data.size(),
             !event.event.empty());
    return event;
}

void SSEParser::reset() {
    m_buffer.clear();
    m_event_count = 0;
}

NDJSONParser::NDJSONParser(LineCallback callback) : m_callback(std::move(callback)) {}

void NDJSONParser::parse(std::string_view chunk) {
    if (chunk.empty()) return;
    m_buffer.append(chunk);
    process_lines();
}

void NDJSONParser::process_lines() {
    if (m_buffer.empty()) return;

    size_t pos = 0;
    size_t last_line_end = 0;

    while (pos < m_buffer.length()) {
        size_t line_end = m_buffer.find('\n', pos);

        if (line_end == std::string::npos) break;

        std::string line = m_buffer.substr(pos, line_end - pos);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty() && m_callback) {
            m_callback(line);
        }

        last_line_end = line_end + 1;
        pos = line_end + 1;
    }

    if (last_line_end > 0) {
        // C.9：substr → erase，避免重新分配
        m_buffer.erase(0, last_line_end);
    }
}

void NDJSONParser::reset() { m_buffer.clear(); }

}  // namespace agent
