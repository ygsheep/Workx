/**
 * @file event_token.h
 * @brief 事件订阅令牌（RAII 友好）
 * @details 与 IEventBus / EventBus 解耦，避免循环包含
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <cstdint>

namespace agent {

class EventToken {
   public:
    using ID = uint64_t;

    EventToken() : m_id(0), m_is_valid(false) {}
    explicit EventToken(ID id) : m_id(id), m_is_valid(true) {}

    EventToken(const EventToken&) = default;
    EventToken& operator=(const EventToken&) = default;

    EventToken(EventToken&& other) noexcept : m_id(other.m_id), m_is_valid(other.m_is_valid) {
        other.m_is_valid = false;
        other.m_id = 0;
    }

    EventToken& operator=(EventToken&& other) noexcept {
        if (this != &other) {
            m_id = other.m_id;
            m_is_valid = other.m_is_valid;
            other.m_is_valid = false;
            other.m_id = 0;
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const { return m_is_valid; }
    [[nodiscard]] ID get_id() const { return m_id; }

    void invalidate() { m_is_valid = false; }

   private:
    ID m_id;
    bool m_is_valid;
};

}  // namespace agent
