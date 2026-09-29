/**
 * @file uuid.h
 * @brief UUIDv4 生成工具
 * @details 用于会话 ID 生成。使用 std::random_device + mt19937_64，
 *          符合 RFC 4122 §4.4（random UUID）。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace core::util {

/// @brief 生成 UUIDv4 字符串（小写，带连字符）
/// @return 形如 "550e8400-e29b-41d4-a716-446655440000"
/// @details 每次调用独立 random_device 播种，避免全局状态。
///          线程安全（无共享状态）。
std::string generate_uuid();

}  // namespace core::util
