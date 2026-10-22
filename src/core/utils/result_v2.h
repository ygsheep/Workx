/**
 * @file result_v2.h
 * @brief ResultV2<T> — 统一结果类型（V2）
 * @details std::variant<T, Error> 的语义化封装，替代 Result<T, std::string>。
 *          不提供 unwrap() 抛异常路径；用 value/value_or/map/and_then 链式调用。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <variant>
#include <utility>
#include <type_traits>
#include "core/utils/error.h"

namespace agent {

/// @brief 统一结果类型（V2）
/// @details std::variant<monostate, T, Error> 的语义化封装，替代 Result<T, std::string>
/// @note 不提供 unwrap() 抛异常路径；用 value/value_or/map/and_then 链式调用
/// @details 使用 std::monostate 作为 variant 首位类型，避免要求 T 默认构造
template <typename T>
class ResultV2 {
    static_assert(!std::is_same_v<T, Error>, "ResultV2<Error> is not allowed");
    static_assert(!std::is_void_v<T>, "Use ResultV2<void> for void results");

   public:
    /// @brief 从 Error 隐式构造（用于错误传播）
    /// @details 允许 `return error;` 在返回 ResultV2<T> 的函数中隐式转换
    /// @note 这是有意为之的，支持 TRY_RESULT_V2 宏的错误传播语义
    ResultV2(Error error) { m_data.template emplace<2>(std::move(error)); }

    /// @brief 成功构造
    static ResultV2 ok(T value) {
        ResultV2 r;
        r.m_data.template emplace<1>(std::move(value));
        return r;
    }

    /// @brief 错误构造
    static ResultV2 err(Error error) {
        ResultV2 r;
        r.m_data.template emplace<2>(std::move(error));
        return r;
    }

    /// @brief 错误构造（便捷：错误码 + 消息）
    static ResultV2 err(Error::Code code, std::string message, std::string context = {}) {
        return err(Error{code, std::move(message), std::move(context)});
    }

    [[nodiscard]] bool is_ok() const noexcept { return m_data.index() == 1; }
    [[nodiscard]] bool is_err() const noexcept { return m_data.index() == 2; }

    /// @brief 获取值引用（仅 ok 时有效，调用方需先 is_ok() 守卫）
    /// @note 不抛异常，err 时为未定义行为（debug 模式 assert）
    [[nodiscard]] T& value() noexcept { return std::get<1>(m_data); }
    [[nodiscard]] const T& value() const noexcept { return std::get<1>(m_data); }

    /// @brief 获取错误引用（仅 err 时有效）
    [[nodiscard]] Error& error() noexcept { return std::get<2>(m_data); }
    [[nodiscard]] const Error& error() const noexcept { return std::get<2>(m_data); }

    /// @brief 获取值或默认值（不抛异常）
    [[nodiscard]] T value_or(T default_value) const {
        return is_ok() ? std::get<1>(m_data) : std::move(default_value);
    }

    /// @brief 映射成功值（functor）
    template <typename F>
    auto map(F&& f) const& -> ResultV2<std::invoke_result_t<F, const T&>> {
        using R = std::invoke_result_t<F, const T&>;
        if (is_ok()) {
            return ResultV2<R>::ok(f(std::get<1>(m_data)));
        }
        return ResultV2<R>::err(std::get<2>(m_data));
    }

    template <typename F>
    auto map(F&& f) && -> ResultV2<std::invoke_result_t<F, T&&>> {
        using R = std::invoke_result_t<F, T&&>;
        if (is_ok()) {
            return ResultV2<R>::ok(f(std::move(std::get<1>(m_data))));
        }
        return ResultV2<R>::err(std::move(std::get<2>(m_data)));
    }

    /// @brief 链式操作（monadic bind）
    /// @details f 接收 T&&，返回 ResultV2<U>
    template <typename F>
    auto and_then(F&& f) && -> std::invoke_result_t<F, T&&> {
        using R = std::invoke_result_t<F, T&&>;
        if (is_ok()) {
            return f(std::move(std::get<1>(m_data)));
        }
        return R::err(std::move(std::get<2>(m_data)));
    }

    /// @brief 错误映射（error functor）
    template <typename F>
    ResultV2 map_err(F&& f) const {
        if (is_err()) {
            return ResultV2::err(f(std::get<2>(m_data)));
        }
        return *this;
    }

   private:
    ResultV2() = default;
    std::variant<std::monostate, T, Error> m_data;
};

/// @brief ResultV2<void> 特化
template <>
class ResultV2<void> {
   public:
    /// @brief 从 Error 隐式构造（用于错误传播）
    ResultV2(Error error) : m_is_ok(false), m_error(std::move(error)) {}

    static ResultV2 ok() {
        ResultV2 r;
        r.m_is_ok = true;
        return r;
    }
    static ResultV2 err(Error error) {
        ResultV2 r;
        r.m_is_ok = false;
        r.m_error = std::move(error);
        return r;
    }
    static ResultV2 err(Error::Code code, std::string message, std::string context = {}) {
        return err(Error{code, std::move(message), std::move(context)});
    }

    [[nodiscard]] bool is_ok() const noexcept { return m_is_ok; }
    [[nodiscard]] bool is_err() const noexcept { return !m_is_ok; }

    [[nodiscard]] const Error& error() const noexcept { return m_error; }
    [[nodiscard]] Error& error() noexcept { return m_error; }

    /// @brief 映射成功值（void → T）
    template <typename F>
    auto map(F&& f) const -> ResultV2<std::invoke_result_t<F>> {
        using R = std::invoke_result_t<F>;
        if (is_ok()) {
            return ResultV2<R>::ok(f());
        }
        return ResultV2<R>::err(m_error);
    }

    /// @brief 链式操作（void → ResultV2<U>）
    template <typename F>
    auto and_then(F&& f) const -> std::invoke_result_t<F> {
        using R = std::invoke_result_t<F>;
        if (is_ok()) {
            return f();
        }
        return R::err(m_error);
    }

    /// @brief 错误映射
    template <typename F>
    ResultV2 map_err(F&& f) const {
        if (is_err()) {
            return ResultV2::err(f(m_error));
        }
        return *this;
    }

   private:
    ResultV2() = default;
    bool m_is_ok = true;
    Error m_error;
};

// ============================================================
// 便捷工具
// ============================================================

/// @brief TRY 宏：错误传播（替代 unwrap）
/// @details auto r = some_op(); if (r.is_err()) return r.error();
///          简化为：TRY_RESULT_V2(var, some_op());
/// @note 使用前需确保外层函数返回 ResultV2 或可隐式转换的类型
#define TRY_RESULT_V2(var, expr) \
    auto var = (expr);           \
    if (var.is_err()) return std::move(var).error()

}  // namespace agent
