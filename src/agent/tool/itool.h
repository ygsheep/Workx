/**
 * @file itool.h
 * @brief ITool 接口 — 工具抽象基类
 * @details 所有 Agent 可调用工具的统一接口：名称、描述、参数 schema、权限检查、输入验证、执行
 *          V2-4：PermissionResult/ValidationResult/call 返回类型迁移到 ResultV2
 *          M-5：按接口隔离原则（ISP）拆分为三个角色接口：
 *               - IToolMetadata：名称、描述、prompt、input_schema（静态元信息）
 *               - IToolGuard：check_permissions / validate_input（前置校验）
 *               - IToolCallable：call（实际执行）
 *               ITool 继承三者作为组合接口，现有工具实现 ITool 即可，向后兼容
 * @version 2.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "core/utils/result_v2.h"
#include "core/utils/error.h"
#include "agent/tool/result.h"
#include "agent/tool/context.h"

namespace agent::tool {

/// @brief 权限检查结果类型（V2）
using PermissionResult = ResultV2<void>;

/// @brief 输入验证结果类型（V2）
using ValidationResult = ResultV2<void>;

/// @brief 工具元信息接口（M-5：ISP 拆分）
/// @details 暴露工具的静态描述信息，供 ToolRegistry 列举 schema、
///          system prompt 拼接、UI 展示工具列表等场景使用。
///          这些方法均为 const noexcept 级别（不抛异常、无副作用），
///          适合在日志/UI/registry 等不需要执行能力的场景按此接口引用。
class IToolMetadata {
   public:
    virtual ~IToolMetadata() = default;

    /// @brief 工具名称
    /// @return 工具名称字符串
    virtual const std::string& name() const = 0;

    /// @brief 工具描述
    /// @return 工具描述字符串
    virtual const std::string& description() const = 0;

    /// @brief 工具提示词（供 LLM 理解工具用途）
    /// @return 提示词字符串
    virtual const std::string& prompt() const = 0;

    /// @brief 获取参数 JSON Schema
    /// @return JSON Schema 描述工具输入参数
    virtual nlohmann::json input_schema() const = 0;

    /// @brief 是否为只读工具（无写/执行副作用）
    /// @details 默认 false。只读工具（如 Read/Glob/Grep/WebFetch/TaskOutput/Skill）
    ///          覆盖返回 true。用于子 Agent 在 Plan（只读）权限模式下只获得只读工具集，
    ///          从工具暴露面层面杜绝写/执行能力，与 check_permissions 形成双重防线。
    virtual bool is_read_only() const { return false; }
};

/// @brief 工具前置校验接口（M-5：ISP 拆分）
/// @details 暴露权限与输入校验，供 ToolExecutor 在调用前做 Guard。
///          与 IToolCallable 分离后，未来可让审计/拦截层只依赖此接口，
///          无需引入执行能力。
class IToolGuard {
   public:
    virtual ~IToolGuard() = default;

    /// @brief 检查工具调用权限（默认允许，子类可覆盖）
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 权限检查结果（错误码建议 PermissionDenied）
    virtual PermissionResult check_permissions(const nlohmann::json& /*input*/,
                                               const ToolContext& /*ctx*/
    ) const {
        return PermissionResult::ok();
    }

    /// @brief 验证工具输入（默认通过，子类可覆盖）
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 验证结果（错误码建议 InvalidInput / MissingArgument）
    virtual ValidationResult validate_input(const nlohmann::json& /*input*/,
                                            const ToolContext& /*ctx*/
    ) const {
        return ValidationResult::ok();
    }
};

/// @brief 工具执行接口（M-5：ISP 拆分）
/// @details 暴露 call() 执行能力。与元信息/校验分离后，
///          测试可注入 StubIToolCallable 仅实现执行逻辑，
///          而不需要重写元信息方法。
class IToolCallable {
   public:
    virtual ~IToolCallable() = default;

    /// @brief 执行工具
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 工具执行结果（V2：ResultV2<ToolResult>，错误携带 Error）
    /// @par 线程安全保证（K-1 / Phase 3）
    /// `call()` 标注为 `const`：工具实例本身无可观察副作用，可被多个线程
    /// 并行调用同一实例。需要缓存可变状态的工具用 `mutable` + mutex 保护。
    /// 跨工具共享状态通过单例（如 FileHistory / FileReadStateTracker）访问，
    /// 这些单例内部已用 mutex 保护。
    virtual ResultV2<ToolResult> call(const nlohmann::json& input,
                                      const ToolContext& ctx) const = 0;
};

/// @brief ITool 接口 — 工具抽象基类（M-5：组合 IToolMetadata/IToolGuard/IToolCallable）
///
/// @details M-5：组合三个角色接口，保留原有 ITool 作为工具实现的统一入口。
///          现有工具继承 ITool 即可获得三个角色，无需改动；新代码可按需依赖
///          更窄的子接口（如日志层只依赖 IToolMetadata）。
///
/// 历史接口：
/// - 名称、描述、参数 schema 声明 → IToolMetadata
/// - 权限检查、输入验证 → IToolGuard
/// - 执行 → IToolCallable
/// - 使用同步返回类型，不用 cppcoro::task
class ITool : public IToolMetadata, public IToolGuard, public IToolCallable {
    // 纯抽象组合，无新增方法。所有虚函数由三个父接口定义。
};

}  // namespace agent::tool
