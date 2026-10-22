/**
 * @file sha256.h
 * @brief SHA-256 哈希算法实现（DS_CACHE L-4）
 * @details 手写 SHA-256（FIPS 180-4），无外部依赖。
 *          用于 prefix_shape 的稳定指纹计算，替代 std::hash。
 *
 *          设计目标：
 *          - 跨编译器确定（MSVC/GCC/Clang 结果一致）
 *          - 跨进程稳定（同一输入任何时刻结果相同）
 *          - 可外部验证（Python hashlib.sha256 / openssl dgst -sha256 复算）
 *          - 对齐 DS_CACHE_OPTIMIZATION_PLAN 第 181 行 "sha256 前 8 字节" 要求
 *
 *          仅用于诊断归因，非密码学场景（不抗长度扩展攻击等）。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace agent::compact {

/// @brief SHA-256 摘要字节数（32 字节 = 256 位）
constexpr size_t SHA256_DIGEST_SIZE = 32;

/// @brief 计算 SHA-256 摘要
/// @param data 输入数据
/// @return 32 字节摘要（std::array<uint8_t, 32> 的等价 std::string，二进制安全）
/// @details 纯函数，无全局状态，线程安全。
///          算法实现严格遵循 FIPS 180-4。
std::string sha256(std::string_view data);

/// @brief 计算 SHA-256 摘要并返回前 N 字节的十六进制字符串
/// @param data 输入数据
/// @param bytes 取前 N 字节（默认 8 字节 = 16 位十六进制，对齐 Plan 要求）
/// @return 小写十六进制字符串（如 "9e107d9d"）
/// @details 用于诊断 hash 显示：8 字节摘要 → 16 字符 hex，碰撞概率 2^-64，
///          远低于会话规模。可由 bytes=32 取完整摘要。
std::string sha256_hex(std::string_view data, size_t bytes = 8);

}  // namespace agent::compact
