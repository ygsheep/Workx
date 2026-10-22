/**
 * @file headless_internal.h
 * @brief headless 内部可测件声明（Issue #77 单元测试用）
 * @details 这些符号原本是 headless.cpp 的翻译单元私有实现（匿名 namespace，
 *          或未在头文件导出），导致 tests/unit/agent/headless/ 无法直接验证
 *          各分支（HL-01 ~ HL-12）。此处集中声明并导出。
 *
 *          ⚠️ **非稳定 API**：仅供单元测试使用，不保证向后兼容。
 *          生产代码请只使用 headless.h 的 run_headless / run_headless_with_provider。
 * @version 1.0.0
 * @date 2026-09
 */

#pragma once

#include <optional>
#include <string>

#include "agent/core/react_loop.h"
#include "agent/headless/headless.h"
#include "agent/tool/context.h"

namespace agent {

/// @internal 权限模式字符串 → PermissionMode
/// @return 空串 / "default" → Default；plan / accept-edits / bypass-permissions 各自映射；
///         未知字符串返回 std::nullopt（调用方据此返回 exit_code 2）
std::optional<tool::PermissionMode> parse_permission_mode(const std::string& s);

/// @internal 从 ReActResult 取展示文本
/// @details 回退链：final_answer → partial_content → error_message → 空串
std::string result_text(const ReActResult& r);

/// @internal 退出码语义：was_error || was_interrupted → 1，否则 0
int derive_exit_code(const ReActResult& r);

/// @internal 按 output_format 把结果序列化进 out
/// @param streamed stream-json 模式下已累积的逐步输出，会前置拼接到最终结果之前
void render_output(const HeadlessOptions& opts, const ReActResult& react_result,
                   const std::string& session_id, std::string streamed, std::string& out);

}  // namespace agent
