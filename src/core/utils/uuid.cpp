/**
 * @file uuid.cpp
 * @brief UUIDv4 生成工具实现
 * @version 1.1.0
 * @date 2026-10
 */

#include "core/utils/uuid.h"

#include <random>
#include <format>
#include <cstdint>

namespace core::util {

std::string generate_uuid() {
    // #131：原先是「每次调用 new 一个 random_device 再用**单个** rd() 播种」。
    //       std::random_device::result_type 是 32 位 unsigned int，
    //       于是整个 128 位输出完全由 32 位种子决定 —— 状态空间被削到 2^32，
    //       而非 RFC 4122 §4.4 要求的 122 位随机。生日界下 1000 次采样碰撞概率
    //       ≈1.2e-4，约 7.7 万个 ID 时就有 50% 概率撞过一次（CI 已实测撞到）。
    //
    //       现改为：每线程只播种一次，用 seed_seq 拼 8 个 rd() 输出（256 位种子），
    //       mt19937_64 的状态空间回到 2^19937-1，输出具备完整的 122 位随机性。
    //       代价：引入 thread_local 状态（原实现刻意"避免全局状态"）—— 但
    //             thread_local 无跨线程共享，且省掉每次开关 /dev/urandom 的开销。
    //       初始化列表的元素求值顺序自 C++11 起保证从左到右，8 次 rd() 顺序确定。
    static thread_local std::mt19937_64 gen = [] {
        std::random_device rd;
        std::seed_seq seed{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seed);
    }();

    // 生成 128 位随机数（两个 64 位）
    // UUID 128 位布局：time_low(32) | time_mid(16) | time_hi(16) | clock_seq(16) | node(48)
    // hi 提供 time_low + time_mid + time_hi（共 64 位）
    // lo 提供 clock_seq + node（共 64 位）
    uint64_t hi = gen();
    uint64_t lo = gen();

    // 从 hi 提取 time_low(32) / time_mid(16) / time_hi(16)
    uint32_t time_low = static_cast<uint32_t>(hi & 0xFFFFFFFF);      // hi 低 32 位
    uint16_t time_mid = static_cast<uint16_t>((hi >> 32) & 0xFFFF);  // hi 的 32-47 位
    uint32_t time_hi = static_cast<uint32_t>((hi >> 48) & 0xFFFF);   // hi 的高 16 位
    // 按 RFC 4122 §4.4 设置版本位：第 7 字节高 4 位 = 0100 (4)
    time_hi = (time_hi & 0x0FFF) | 0x4000;

    // 从 lo 提取 clock_seq(16) / node(48)
    uint16_t clock_seq = static_cast<uint16_t>(lo & 0xFFFF);  // lo 低 16 位
    // 按 RFC 4122 §4.4 设置变体位：第 9 字节高 2 位 = 10
    clock_seq = (clock_seq & 0x3FFF) | 0x8000;
    uint64_t node = (lo >> 16) & 0xFFFFFFFFFFFFULL;  // lo 的 16-63 位（48 位）

    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}", time_low, time_mid, time_hi,
                       clock_seq, node);
}

}  // namespace core::util
