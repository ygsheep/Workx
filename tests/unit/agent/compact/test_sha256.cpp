/**
 * @file test_sha256.cpp
 * @brief SHA-256 实现单元测试（DS_CACHE L-4）
 * @details 使用 NIST FIPS 180-4 官方测试向量验证实现正确性
 */

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "agent/compact/sha256.h"

using namespace agent::compact;

// ============================================================
// NIST FIPS 180-4 官方测试向量
// ============================================================

TEST_CASE("sha256: empty string (NIST test 1)", "[compact][sha256][ds_cache]") {
    // NIST FIPS 180-4 B.1: "" → e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
    std::string digest = sha256("");
    REQUIRE(digest.size() == SHA256_DIGEST_SIZE);

    std::string hex = sha256_hex("", 32);
    REQUIRE(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("sha256: 'abc' (NIST test 2)", "[compact][sha256][ds_cache]") {
    // NIST FIPS 180-4 B.2: "abc" → ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    std::string hex = sha256_hex("abc", 32);
    REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256: 448-bit message (NIST test 3)", "[compact][sha256][ds_cache]") {
    // NIST FIPS 180-4 B.3: "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
    // → 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
    std::string input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    std::string hex = sha256_hex(input, 32);
    REQUIRE(hex == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("sha256: 896-bit message (two blocks, NIST test 4)", "[compact][sha256][ds_cache]") {
    // NIST FIPS 180-4 B.4: "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn..."
    // (896 位 = 112 字节，需两个块处理)
    std::string input =
        "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmno"
        "ijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    std::string hex = sha256_hex(input, 32);
    REQUIRE(hex == "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1");
}

// ============================================================
// 前 8 字节截断（Plan 要求）
// ============================================================

TEST_CASE("sha256: hex truncation to 8 bytes (16 chars)", "[compact][sha256][ds_cache]") {
    // 前 8 字节 = 16 个十六进制字符
    std::string hex = sha256_hex("abc", 8);
    REQUIRE(hex.size() == 16);
    REQUIRE(hex == "ba7816bf8f01cfea");

    // 默认参数 = 8 字节
    std::string hex_default = sha256_hex("abc");
    REQUIRE(hex_default == hex);
}

// ============================================================
// 跨调用稳定性（确定性）
// ============================================================

TEST_CASE("sha256: deterministic across calls", "[compact][sha256][ds_cache]") {
    std::string h1 = sha256_hex("test input", 32);
    std::string h2 = sha256_hex("test input", 32);
    REQUIRE(h1 == h2);

    // 不同输入产生不同 hash
    std::string h3 = sha256_hex("different input", 32);
    REQUIRE(h1 != h3);
}

// ============================================================
// 边界情况
// ============================================================

TEST_CASE("sha256: boundary lengths around block size", "[compact][sha256][ds_cache]") {
    // 测试不同长度（55/56/64/65 字节边界，覆盖填充逻辑）
    // 主要验证不崩溃且结果稳定
    for (size_t len : {0, 1, 55, 56, 63, 64, 65, 127, 128, 129}) {
        std::string input(len, 'x');
        std::string hex = sha256_hex(input, 8);
        REQUIRE(hex.size() == 16);
        // 同长度同内容应稳定
        REQUIRE(sha256_hex(input, 8) == hex);
    }
}

TEST_CASE("sha256: binary-safe (null bytes in input)", "[compact][sha256][ds_cache]") {
    // 含 null 字节的输入（二进制安全）
    std::string input = "hello\0world";
    input.resize(11);  // 截断到 11 字节（含中间 null）
    std::string hex = sha256_hex(input, 8);
    REQUIRE(hex.size() == 16);

    // 与不含 null 的版本不同
    std::string hex_no_null = sha256_hex("helloworld", 8);
    REQUIRE(hex != hex_no_null);
}
