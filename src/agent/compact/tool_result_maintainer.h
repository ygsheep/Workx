/**
 * @file tool_result_maintainer.h
 * @brief tool_result 两级维护：snip（截短）+ prune（删除）
 * @details DS_CACHE_OPTIMIZATION_PLAN 层次 4。
 *          在昂贵的 LLM 摘要之前，用机械方式释放空间：
 *          - snip：保留头/尾若干行，中段替换为占位符
 *          - prune：整段替换为占位符（更激进）
 *          不调用 LLM，零额外成本。
 *          不删除消息，不改 tool_call_id，不改 assistant content，
 *          保证 tool/assistant 消息配对结构完整（避免 API 报错）。
 *
 *          策略按工具语义分类（对齐 Reasonix prune.go）：
 *          - 只读工具（Read/Glob/Grep）：头长尾短（答案在前）
 *          - 副作用工具（Write/Edit/Bash）：头尾均等（错误可能在尾）
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/api/chat_types.h"

namespace agent::compact {

/// @brief snip/prune 策略
/// @details head/tail 为保留的行数；head_chars/tail_chars 为保留的字符数（行数 fallback）
struct SnipStrategy {
    int head_lines = 80;      ///< 保留头部行数
    int tail_lines = 12;      ///< 保留尾部行数
    int head_chars = 10'000;  ///< 头部字符上限（行数 fallback 时使用）
    int tail_chars = 2'000;   ///< 尾部字符上限
};

/// @brief snip/prune 操作统计
struct SnipStats {
    int results = 0;      ///< 处理的 tool_result 数
    int saved_chars = 0;  ///< 节省的字符数
    std::string archive;  ///< 归档路径（暂未使用，预留）
};

/// @brief 默认只读工具 snip 策略（对齐 Reasonix defaultReadOnlySnip）
/// @details 答案通常在前部：头长尾短
const SnipStrategy& default_read_only_snip();

/// @brief 默认副作用工具 snip 策略（对齐 Reasonix defaultSideEffectingSnip）
/// @details 错误信息可能在尾部：头尾均等
const SnipStrategy& default_side_effecting_snip();

/// @brief 按 tool_name 选择 snip 策略
/// @param tool_name 工具名（如 "Read" / "Bash" / "Grep"）
/// @return 对应策略引用
/// @details 已知只读工具：Read/Glob/Grep/LS/WebFetch
///          已知副作用工具：Write/Edit/Bash/PowerShell/Agent
///          未知工具：按只读处理（默认头长尾短）
const SnipStrategy& select_strategy_by_tool_name(const std::string& tool_name);

/// @brief 判定工具是否为只读（用于选择 snip 策略）
/// @details 公开以便测试。当前硬编码列表，未来可由工具自声明
bool is_read_only_tool(const std::string& tool_name);

/// @brief snip 单条 tool_result：保留头/尾行，中段替换为占位符
/// @param msg 待截短的 tool_result 消息（必须 role==Tool，会被原地修改）
/// @param strategy snip 策略
/// @return 实际节省的字符数（截短前 length - 截短后 length）
/// @details 占位符格式：[snipped tool result — X chars elided]
///          若原内容已短于 head+tail 行总字符数，则不截短
int snip_tool_result(ChatMessage& msg, const SnipStrategy& strategy);

/// @brief prune 单条 tool_result：整段替换为占位符
/// @param msg 待删除内容的 tool_result 消息（必须 role==Tool，会被原地修改）
/// @param archive_dir 归档目录（暂未使用，占位符中标注）
/// @return 实际节省的字符数
/// @details 占位符格式：[elided tool result — X bytes]
///          比 snip 更激进，仅在 force 水位时使用
int prune_tool_result(ChatMessage& msg, const std::string& archive_dir = "");

/// @brief 批量 snip：对 [head_end, tail_start) 区间内的所有 tool_result 执行 snip
/// @param messages 消息列表（会被原地修改）
/// @param head_end 头部边界（不含）
/// @param tail_start 尾部边界（不含）
/// @param strategy_getter 策略选择器（按 tool_name 返回策略），传 nullptr 用默认
/// @return 操作统计
SnipStats snip_range(std::vector<ChatMessage>& messages, size_t head_end, size_t tail_start,
                     const std::string& archive_dir = "");

/// @brief 批量 prune：对 [head_end, tail_start) 区间内的所有 tool_result 执行 prune
/// @details 比 snip 更激进，仅在 force 水位时使用
SnipStats prune_range(std::vector<ChatMessage>& messages, size_t head_end, size_t tail_start,
                      const std::string& archive_dir = "");

}  // namespace agent::compact
