/**
 * @file action_queue.h
 * @brief ActionQueue — 并发安全队列（后台写 / UI 线程读）
 */

#pragma once

#include <deque>
#include <mutex>
#include "bridge/action.h"

namespace ftxtui {

/// @brief 单生产者（事件回调线程）+ 单消费者（UI 线程）的动作队列
/// @details 事件回调入队不阻塞、不持锁碰 UI；UI 线程每帧 drain。
///          使用任意线程安全的队列即可，这里用互斥 + deque。
class ActionQueue {
   public:
    /// @brief 入队一个动作（任意线程可调用）
    void push(Action action);

    /// @brief 取走队列中所有动作（UI 线程调用）
    /// @return 已取出的动作列表（保持入队顺序）
    std::deque<Action> drain();

    /// @brief 是否为空
    bool empty() const;

   private:
    mutable std::mutex m_mutex;
    std::deque<Action> m_queue;
};

}  // namespace ftxtui