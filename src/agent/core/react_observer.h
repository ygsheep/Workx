/**
 * @file react_observer.h
 * @brief IReActObserver — ReAct 循环观察者接口
 * @details 3.2：解耦 ChatSession 与 ReActLoop 的事件回调。
 *          ReActLoop 通过观察者接口发布步骤事件，ChatSession 实现该接口
 *          做 EventBus 事件转换，使 ReActLoop 可脱离 EventBus 体系独立使用。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/core/react_loop.h"
#include "core/export.h"

namespace agent {

/// @brief ReAct 循环观察者接口
/// @details 所有方法由 ReActLoop 在对应阶段同步调用，实现方应避免阻塞。
///          默认空实现，子类按需覆盖。
class WORKX_API IReActObserver {
   public:
    virtual ~IReActObserver() = default;

    /// @brief Thought 阶段完成（LLM 推理 + 工具调用决策）
    virtual void on_thought(const ReActStep& /*step*/) {}

    /// @brief Action 阶段（工具调用开始）
    virtual void on_action(const ReActStep& /*step*/) {}

    /// @brief Observation 阶段（工具结果回传）
    virtual void on_observation(const ReActStep& /*step*/) {}

    /// @brief FinalAnswer 阶段（LLM 给出最终回复，循环即将退出）
    virtual void on_final_answer(const ReActStep& /*step*/) {}

    /// @brief 流式 token 增量（仅 Thought 阶段 LLM 输出 delta）
    virtual void on_token(const std::string& /*content_delta*/,
                          const std::string& /*reasoning_delta*/) {}
};

}  // namespace agent
