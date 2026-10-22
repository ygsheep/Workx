/**
 * @file sha256.cpp
 * @brief SHA-256 哈希算法实现（DS_CACHE L-4）
 * @details 严格遵循 FIPS 180-4 规范，无外部依赖。
 *          参考：NIST FIPS 180-4, RFC 6234
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/compact/sha256.h"

#include <array>
#include <cstring>

namespace agent::compact {

namespace {

/// SHA-256 常量：前 32 位小素数立方根的小数部分（FIPS 180-4 §4.2.2）
constexpr std::array<uint32_t, 64> kK = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

/// SHA-256 初始哈希值：前 8 位素数平方根的小数部分（FIPS 180-4 §5.3.3）
constexpr std::array<uint32_t, 8> kH0 = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                         0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

/// 循环右移
inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

/// SHA-256 函数（FIPS 180-4 §4.1.2）
inline uint32_t Ch(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (~x & z); }
inline uint32_t Maj(uint32_t x, uint32_t y, uint32_t z) { return (x & y) ^ (x & z) ^ (y & z); }
inline uint32_t Sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t Sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t sigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t sigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

/// 大端序写入 32 位整数
inline void write_be32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

/// 大端序读取 32 位整数
inline uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// 处理单个 512 位（64 字节）块
void process_block(const uint8_t* block, std::array<uint32_t, 8>& h) {
    std::array<uint32_t, 64> w{};

    // 1. 准备消息调度表 W[0..63]（FIPS 180-4 §6.2.2 步骤 1）
    for (size_t t = 0; t < 16; ++t) {
        w[t] = read_be32(block + t * 4);
    }
    for (size_t t = 16; t < 64; ++t) {
        w[t] = sigma1(w[t - 2]) + w[t - 7] + sigma0(w[t - 15]) + w[t - 16];
    }

    // 2. 初始化工作变量（步骤 2）
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

    // 3. 主循环（步骤 3）
    for (size_t t = 0; t < 64; ++t) {
        uint32_t t1 = hh + Sigma1(e) + Ch(e, f, g) + kK[t] + w[t];
        uint32_t t2 = Sigma0(a) + Maj(a, b, c);
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    // 4. 计算中间哈希值（步骤 4）
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

}  // anonymous namespace

std::string sha256(std::string_view data) {
    // 初始化哈希值
    std::array<uint32_t, 8> h = kH0;

    const auto* p = reinterpret_cast<const uint8_t*>(data.data());
    size_t len = data.size();

    // 处理完整的 64 字节块
    while (len >= 64) {
        process_block(p, h);
        p += 64;
        len -= 64;
    }

    // 填充：1 位 1 + k 位 0 + 64 位长度（FIPS 180-4 §5.1.1）
    // 布局：[剩余数据][0x80][0...0][bit_len 大端 8 字节]
    std::array<uint8_t, 128> padding{};
    size_t pad_len = 0;

    // 先复制剩余数据到 padding 开头
    if (len > 0) {
        std::memcpy(padding.data(), p, len);
    }
    // 紧跟 0x80（首位 1）
    padding[len] = 0x80;
    // 计算填充长度：总长需对齐 64 字节，留 8 字节给 bit_len
    pad_len = (len < 56) ? 56 : 120;

    // 附加原始长度（位，大端序）
    uint64_t bit_len = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 0; i < 8; ++i) {
        padding[pad_len + i] = static_cast<uint8_t>((bit_len >> (56 - i * 8)) & 0xFF);
    }

    // 处理填充块（1 或 2 个）
    process_block(padding.data(), h);
    if (pad_len == 120) {
        process_block(padding.data() + 64, h);
    }

    // 输出大端序字节
    std::string digest(SHA256_DIGEST_SIZE, '\0');
    for (size_t i = 0; i < 8; ++i) {
        write_be32(reinterpret_cast<uint8_t*>(digest.data()) + i * 4, h[i]);
    }
    return digest;
}

std::string sha256_hex(std::string_view data, size_t bytes) {
    if (bytes == 0 || bytes > SHA256_DIGEST_SIZE) {
        bytes = SHA256_DIGEST_SIZE;
    }

    std::string digest = sha256(data);
    static const char hex_chars[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t byte = static_cast<uint8_t>(digest[i]);
        result.push_back(hex_chars[(byte >> 4) & 0x0F]);
        result.push_back(hex_chars[byte & 0x0F]);
    }
    return result;
}

}  // namespace agent::compact
