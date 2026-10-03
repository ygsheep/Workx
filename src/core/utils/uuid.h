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
/// @details #131：每线程只播种一次（thread_local），种子由 8 个 std::random_device
///          输出经 seed_seq 拼成 —— 保证 RFC 4122 §4.4 要求的 122 位随机性。
///          （原实现每次调用只用单个 32 位 rd() 播种，状态空间被削到 2^32。）
///          线程安全：thread_local，无跨线程共享状态。
std::string generate_uuid();

}  // namespace core::util
