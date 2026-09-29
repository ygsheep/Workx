/**
 * @file executor.h
 * @brief ToolExecutor — 工具执行器
 * @details 负责查找工具、权限检查、输入验证、执行工具并返回结果
 *          V2-4：execute() 返回 ResultV2<ExecutionResult>，错误携带 Error
 *          M-3：execute() 拆分为 lookup_tool / run_with_safety / finalize_result
 *               三个职责单一的私有方法，便于独立测试与未来替换日志策略
 * @version 2.2.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <thread>

#include "core/export.h"
#include <filesystem>
#include <utility>
#include <nlohmann/json.hpp>
#include "liblogger/logger.h"
#include "core/utils/result_v2.h"
#include "core/utils/error.h"
#include "itool.h"
#include "registry.h"
#include "result.h"
#include "context.h"
#include "agent/tool/encoding.h"
#include "agent/util/json_schema.h"  // #80 统一 schema 校验
#include "agent/audit/audit_logger.h"
#include "agent/hook/hook_event.h"    // #50 通用 Hook 事件系统：PermissionRequest
#include "agent/hook/hook_manager.h"  // HookManager::dispatch

namespace agent::tool {

/// @brief 工具结果最大长度（超出时截断保留头尾）
/// @details 3.4：防止 grep 大仓库 / bash 长日志撑爆上下文。
///          截断策略：保留头部和尾部（通常包含错误信息和总结），省略中间。
constexpr size_t MAX_TOOL_RESULT_LENGTH = 8000;

/// @brief 工具执行结果（V2：成功载荷）
/// @details
/// V2-4 迁移：移除 is_error 字段，错误状态由 ResultV2<ExecutionResult> 承载。
/// 字段语义：
/// - `tool_name`：上下文信息，标识本次执行调用的工具（用于日志/UI 展示）
/// - `result`：工具实际返回的成功结果（ToolResult）
/// - `was_truncated`：结果元信息，标记 `result.text` 是否被截断过
struct ExecutionResult {
    std::string tool_name;      ///< 上下文：工具名称
    ToolResult result;          ///< 工具返回结果（成功载荷）
    bool was_truncated{false};  ///< 元信息：result.text 是否被截断

    /// @brief 结果是否被截断
    bool is_truncated() const noexcept { return was_truncated; }

    /// @brief 转换为 LLM 可读的文本（委托给 ToolResult::to_string）
    std::string to_string() const { return result.to_string(); }
};

/// @brief 截断工具结果文本（保留头尾，省略中间）
/// @param text 待截断文本
/// @param max_length 最大保留长度
/// @return {截断后的文本, 是否发生了截断}
/// @details L-2：原签名 `bool truncate_result(std::string& text, ...)` 直接修改入参，
///          违反纯函数原则。现改为返回 std::pair，无副作用，可独立测试与复用。
///          UTF-8 安全：截断点回退到字符边界，避免在多字节字符中间截断产生无效 UTF-8。
inline std::pair<std::string, bool> truncate_result(std::string_view text,
                                                    size_t max_length = MAX_TOOL_RESULT_LENGTH) {
    if (text.length() <= max_length) {
        return {std::string{text}, false};
    }
    const size_t half = max_length / 2;

    // UTF-8 安全截断：回退到字符边界（连续字节 10xxxxxx 之前）
    auto safe_boundary = [](std::string_view s, size_t pos) -> size_t {
        if (pos >= s.size()) return s.size();
        // 回退直到 pos 指向非 continuation byte（即字符起始）
        while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        return pos;
    };

    const size_t head_end = safe_boundary(text, half);
    const size_t tail_start = safe_boundary(text, text.length() - half);
    const size_t omitted = text.length() - head_end - (text.length() - tail_start);

    std::string result;
    result.reserve(head_end + (text.length() - tail_start) + 64);
    result.append(text.substr(0, head_end));
    result.append("\n\n... [output truncated, ");
    result.append(std::to_string(omitted));
    result.append(" characters omitted] ...\n\n");
    result.append(text.substr(tail_start));
    return {result, true};
}

// M-B：ExecutionTrace 已删除（Round 1 审查指出为死代码）。
// M-3 的"日志策略可替换"目标留待后续 issue 引入 IToolExecutorObserver 时实现。

/// @brief ToolExecutor — 工具执行器
///
/// 负责实际执行 LLM 请求的工具调用：
/// - 按名称查找工具
/// - 权限检查、输入验证
/// - 调用工具并返回结果
/// - 使用同步返回类型，不用 cppcoro::task
///
/// M-3：execute() 内部委托给三个职责单一的私有方法：
///   - lookup_tool()：纯查找，无副作用
///   - run_with_safety()：try-catch 包装工具调用，返回 ResultV2<ToolResult>
///   - finalize_result()：组装 ExecutionResult + 截断 + 日志
class WORKX_API ToolExecutor {
   public:
    /// @brief 构造函数
    /// @param registry 工具注册表
    explicit ToolExecutor(std::shared_ptr<ToolRegistry> registry)
        : registry_(std::move(registry)) {}

    /// @brief 执行工具（M-3：内部委托 lookup_tool / run_with_safety / finalize_result）
    /// @param tool_name 工具名称
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 执行结果（V2：ResultV2<ExecutionResult>，错误携带 Error）
    inline ResultV2<ExecutionResult> execute(const std::string& tool_name,
                                             const nlohmann::json& input,
                                             const ToolContext& ctx) const {
        // 仅在可能抛出的序列化处加保护（工具入参可能含非 UTF-8 字节）
        size_t input_size = 0;
        try {
            input_size = input.dump().size();
        } catch (const std::exception&) {
            input_size = 0;
        }

        LOG_INFO("[tool_executor] begin, tool={}, input_size={}, thread={}", tool_name, input_size,
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));

        const auto t0 = std::chrono::steady_clock::now();

        // 1. 查找工具
        auto tool = lookup_tool(tool_name);
        if (!tool) {
            LOG_WARN("[tool_executor] tool not found: {}", tool_name);
            audit::AuditLogger::instance().log_tool_invoke(
                tool_name, input, ctx.session_id, ctx.request_id, "deny", "tool not found", 0);
            return Error{Error::Code::ResourceNotFound, "Tool not found: " + tool_name, tool_name};
        }

        // 1.5 极简模式守卫：仅放行白名单工具（Skill/Bash/Read/Write/Edit）
        //     第二道防线——schema 过滤后 LLM 仍可能幻觉出其他工具名，这里直接拒绝。
        if (ctx.session_mode == SessionMode::Minimal && !is_minimal_mode_tool(tool_name)) {
            LOG_WARN("[tool_executor] tool={} denied by minimal mode", tool_name);
            audit::AuditLogger::instance().log_tool_invoke(tool_name, input, ctx.session_id,
                                                           ctx.request_id, "deny",
                                                           "minimal mode: tool not allowed", 0);
            return Error{Error::Code::PermissionDenied,
                         "Tool '" + tool_name +
                             "' is not available in minimal mode "
                             "(only Skill/Bash/Read/Write/Edit)",
                         tool_name};
        }

        // 2. 检查取消信号
        if (ctx.is_cancelled()) {
            LOG_INFO("[tool_executor] tool={} cancelled before execution", tool_name);
            audit::AuditLogger::instance().log_tool_invoke(tool_name, input, ctx.session_id,
                                                           ctx.request_id, "deny",
                                                           "cancelled before execution", 0);
            return Error{Error::Code::Cancelled, "Tool execution cancelled", tool_name};
        }

        // 2.5 #50 PermissionRequest hook：在常规权限检查前派发，hook 可动态授权/阻断
        // 匹配已由 HookManager 内 HookMatcher 完成（event+tool_name），未匹配则零开销返回。
        if (ctx.hook_manager_ptr && !ctx.hook_manager_ptr->empty()) {
            hook::HookContext hctx;
            hctx.session_id = ctx.session_id;
            hctx.cwd = ctx.cwd;
            hctx.request_id = ctx.request_id;
            hctx.tool_name = tool_name;
            hctx.tool_input = input;
            auto hres = ctx.hook_manager_ptr->dispatch(hook::HookEvent::PermissionRequest, hctx);
            if (hres.blockingError && !hres.blockingError->empty()) {
                LOG_WARN("[tool_executor] tool={} blocked by PermissionRequest hook: {}", tool_name,
                         *hres.blockingError);
                audit::AuditLogger::instance().log_tool_invoke(tool_name, input, ctx.session_id,
                                                               ctx.request_id, "deny",
                                                               *hres.blockingError, 0);
                return Error{Error::Code::PermissionDenied, *hres.blockingError, tool_name};
            }
        }

        // 3. 权限检查
        auto perm = tool->check_permissions(input, ctx);
        if (perm.is_err()) {
            LOG_WARN("[tool_executor] tool={} permission denied: {}", tool_name,
                     perm.error().message);
            audit::AuditLogger::instance().log_tool_invoke(
                tool_name, input, ctx.session_id, ctx.request_id, "deny", perm.error().message, 0);
            return perm.error();
        }

        // 4. 输入验证（工具手写的语义校验优先）
        auto validation = tool->validate_input(input, ctx);
        if (validation.is_err()) {
            LOG_WARN("[tool_executor] tool={} invalid input: {}", tool_name,
                     validation.error().message);
            audit::AuditLogger::instance().log_tool_invoke(
                tool_name, input, ctx.session_id, ctx.request_id, "deny",
                "invalid input: " + validation.error().message, 0);
            return validation.error();
        }

        // 4.5 统一 schema 校验（#80）：作为兜底，仅当工具未通过手写校验拦截时执行。
        //     这样既保留工具定制的精确错误信息，又为"只声明 schema、不写
        //     validate_input"的工具补位（默认 validate_input 无条件返回 ok）。
        //     仅当工具声明了非空 input_schema 时执行；错误码细分：
        //     缺必填 → MissingArgument，其余（类型/枚举/范围/pattern）→ InvalidInput。
        auto schema = tool->input_schema();
        if (!schema.is_null() && !schema.empty()) {
            auto sres = agent::util::validate_schema(schema, input, tool_name);
            if (!sres.ok) {
                // 判定是否存在"缺必填"错误，映射对应错误码
                bool has_missing = false;
                for (const auto& e : sres.errors) {
                    if (e.is_missing) {
                        has_missing = true;
                        break;
                    }
                }
                const auto code =
                    has_missing ? Error::Code::MissingArgument : Error::Code::InvalidInput;
                const std::string msg = sres.to_string();
                LOG_WARN("[tool_executor] tool={} schema validation failed: {}", tool_name, msg);
                audit::AuditLogger::instance().log_tool_invoke(tool_name, input, ctx.session_id,
                                                               ctx.request_id, "deny",
                                                               "schema invalid: " + msg, 0);
                return Error{code, msg, tool_name};
            }
        }

        // 5. 执行工具（try-catch 包装），返回 ToolResult 或 Error
        auto call_result = run_with_safety(*tool, tool_name, input, ctx);
        if (call_result.is_err()) {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            audit::AuditLogger::instance().log_tool_invoke(tool_name, input, ctx.session_id,
                                                           ctx.request_id, "allow", "", ms, "", {});
            return call_result.error();
        }

        // 6. 组装结果 + 截断 + 日志
        return finalize_result(tool_name, std::move(call_result).value(), input, ctx, t0);
    }

   private:
    std::shared_ptr<ToolRegistry> registry_;

    /// @brief 查找工具（L-B：纯查找，无日志副作用）
    /// @param tool_name 工具名称
    /// @return 工具实例（未找到返回 nullptr；日志由 execute() 入口统一记录）
    inline std::shared_ptr<ITool> lookup_tool(const std::string& tool_name) const {
        return registry_->find_by_name(tool_name);
    }

    /// @brief 在 try-catch 包装下执行工具调用
    /// @details 捕获 json/filesystem/bad_alloc/std::exception/未知异常，统一转为 Error
    /// @note H-A：参数类型改为 IToolCallable&（M-5 ISP），明确本方法仅需执行能力，
    ///       不访问元信息或 Guard。ITool 继承 IToolCallable，调用方传 ITool& 可隐式绑定。
    /// @return 成功返回 ToolResult；失败返回 Error
    inline ResultV2<ToolResult> run_with_safety(IToolCallable& tool, const std::string& tool_name,
                                                const nlohmann::json& input,
                                                const ToolContext& ctx) const {
        auto t0 = std::chrono::steady_clock::now();
        try {
            auto call_result = tool.call(input, ctx);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
            if (call_result.is_err()) {
                LOG_INFO("[tool_executor] tool={} end, error, duration={}ms", tool_name, ms);
                return call_result.error();
            }
            LOG_INFO("[tool_executor] tool={} end, ok, duration={}ms", tool_name, ms);
            return call_result;
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("[tool_executor] tool={} JSON exception: {}", tool_name, e.what());
            return Error{Error::Code::ToolExecutionFailed,
                         std::string{"JSON error in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (const std::filesystem::filesystem_error& e) {
            LOG_ERROR("[tool_executor] tool={} filesystem exception: {}", tool_name, e.what());
            return Error{Error::Code::ToolExecutionFailed,
                         std::string{"Filesystem error in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("[tool_executor] tool={} bad_alloc: {}", tool_name, e.what());
            return Error{Error::Code::InternalError,
                         std::string{"Out of memory in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (const std::exception& e) {
            LOG_ERROR("[tool_executor] tool={} std::exception: {}", tool_name, e.what());
            return Error{Error::Code::ToolExecutionFailed,
                         std::string{"Error in tool '"} + tool_name + "': " + e.what(), tool_name};
        } catch (...) {
            LOG_ERROR("[tool_executor] tool={} unknown exception", tool_name);
            return Error{Error::Code::Unknown,
                         std::string{"Unknown exception in tool '"} + tool_name + "'", tool_name};
        }
    }

    /// @brief 组装 ExecutionResult 并执行截断
    /// @param tool_name 工具名称
    /// @param result 工具返回的 ToolResult
    /// @param input 工具输入（用于审计日志）
    /// @param ctx 工具上下文（用于审计日志）
    /// @param t0 起始时间点（用于审计日志耗时计算）
    /// @return 最终的 ExecutionResult
    inline ResultV2<ExecutionResult> finalize_result(
        const std::string& tool_name, ToolResult result, const nlohmann::json& input,
        const ToolContext& ctx, std::chrono::steady_clock::time_point t0) const {
        ExecutionResult exec_result;
        exec_result.tool_name = tool_name;
        exec_result.result = std::move(result);

        // UTF-8 清洗：去除工具输出中的非法 UTF-8 字节（如 GBK 子进程 stdout），
        // 避免下游 json 序列化（审计/to_string）抛 type_error.316
        exec_result.result.text = sanitize_utf8(exec_result.result.text);

        // 3.4：结果截断（防止 grep/bash 长输出撑爆上下文）
        if (!exec_result.result.text.empty() &&
            exec_result.result.text.length() > MAX_TOOL_RESULT_LENGTH) {
            auto [truncated_text, was_truncated] = truncate_result(exec_result.result.text);
            exec_result.result.text = std::move(truncated_text);
            exec_result.was_truncated = was_truncated;
            LOG_INFO("[tool_executor] tool={} result truncated, new_len={}", tool_name,
                     exec_result.result.text.length());
        }

        // 审计日志：记录工具调用结果
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        audit::AuditLogger::instance().log_tool_invoke(tool_name, input, ctx.session_id,
                                                       ctx.request_id, "allow", "", ms,
                                                       exec_result.result.text);

        LOG_INFO(
            "[tool_executor] done, tool={}, elapsed_ms={}, result_len={}, "
            "truncated={}, thread={}",
            tool_name, ms, exec_result.result.text.length(), exec_result.was_truncated,
            std::hash<std::thread::id>{}(std::this_thread::get_id()));

        return ResultV2<ExecutionResult>::ok(std::move(exec_result));
    }
};

}  // namespace agent::tool
