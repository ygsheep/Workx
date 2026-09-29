/**
 * @file sse_parser.hpp
 * @brief Server-Sent Events (SSE) + NDJSON 流式解析器
 * @details 增量解析，自动缓冲不完整数据块
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <string_view>
#include <functional>
#include <sstream>
#include <map>
#include <optional>

namespace agent {

struct SSEEvent {
    std::string data;
    std::string event;
    std::string id;
    std::optional<int> retry;  ///< 缺省表示未设置；retry:0 是合法值（立即重连）

    bool has_data() const { return !data.empty(); }
    bool has_event() const { return !event.empty(); }
    bool has_id() const { return !id.empty(); }
    bool has_retry() const { return retry.has_value(); }
};

class SSEParser {
   public:
    using EventCallback = std::function<void(const SSEEvent&)>;

    explicit SSEParser(EventCallback callback);
    /// @brief 喂入原始 SSE 数据块（C.10：改 string_view 避免调用方拷贝）
    /// @details parser 内部立即 append 到 m_buffer，不持有 view 生命周期
    void parse(std::string_view chunk);
    void reset();

    const std::string& get_buffer() const { return m_buffer; }
    size_t get_event_count() const { return m_event_count; }

   private:
    EventCallback m_callback;
    std::string m_buffer;
    size_t m_event_count = 0;

    void process_events();
    static SSEEvent parse_event(std::string_view event_text);
};

class NDJSONParser {
   public:
    using LineCallback = std::function<void(const std::string&)>;

    explicit NDJSONParser(LineCallback callback);
    void parse(std::string_view chunk);
    void reset();

   private:
    LineCallback m_callback;
    std::string m_buffer;

    void process_lines();
};

}  // namespace agent
