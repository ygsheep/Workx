/**
 * @file message_node.h
 * @brief MessageNode — 单条消息的视图模型（UI 线程独有）
 * @details 不承载格式化/渲染，仅保存原始数据与展示状态。
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ftxtui {

enum class MsgRole { User, Assistant, Error };

/// @brief 一个工具调用块（可折叠）
struct ToolCallNode {
    std::string tool_name;
    std::string call_id;
    std::string arguments;  ///< JSON 参数字符串
    std::string result;     ///< 工具返回文本
    bool running = false;   ///< 执行中（显示 spinner）
    bool done = false;      ///< 已返回
    bool is_error = false;  ///< 出错
    bool expanded = false;  ///< 结果是否展开
    std::size_t text_pos =
        0;  ///< 该工具调用在正文 text 中的插入位置（字节偏移，用于与正文交错渲染）
    std::string sub_task_id;  ///< 关联的子 Agent 任务 id（Agent 工具卡跳转第二层用；空=非子 Agent）
};

/// @brief 一条消息
struct MessageNode {
    MsgRole role = MsgRole::Assistant;
    std::string text;        ///< 正文（流式期间持续追加）
    std::string reasoning;   ///< 思考内容（默认折叠展示）
    bool streaming = false;  ///< 正在流式输出
    bool reasoned = false;   ///< 是否收到过思考增量（决定是否渲染思考折叠块）
    bool reasoning_expanded = false;  ///< 思考内容是否展开
    bool sealed = false;              ///< turn 已结束，后续 token/工具不再追加到本条
    bool notice = false;  ///< UI 追加的非模型回复文本（通知/命令输出等），不显示操作按钮栏
    std::vector<ToolCallNode> tool_calls;
    std::vector<std::string> tool_use_ids;  ///< 当前已创建块的顺序（用于去重）

    // 统计
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    int32_t cache_read_tokens = 0;
    double duration_ms = 0.0;
    double reasoning_ms = 0.0;  ///< 思考阶段耗时（思考折叠标签显示）

    /// @brief 查找工具块
    ToolCallNode* find_tool(const std::string& call_id);
    const ToolCallNode* find_tool(const std::string& call_id) const;
};

}  // namespace ftxtui