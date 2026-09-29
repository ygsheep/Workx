/**
 * @file test_ssrf.cpp
 * @brief SSRF 防护原语单元测试（#25）
 */

#include <catch2/catch_test_macros.hpp>
#include <string>

#include "agent/api/remote/http_client.h"
#include "agent/api/remote/ssrf.h"
#include "core/utils/error.h"

using namespace agent;

// ============================================================================
// IPv4 内网/回环/链路本地判定
// ============================================================================

TEST_CASE("is_private_ipv4 拦截内网/回环/链路本地", "[ssrf][ipv4]") {
    // 10/8
    REQUIRE(is_private_ipv4(0x0A000001u));  // 10.0.0.1
    REQUIRE(is_private_ipv4(0x0AFFFFFFu));  // 10.255.255.255
    // 172.16/12
    REQUIRE(is_private_ipv4(0xAC100001u));  // 172.16.0.1
    REQUIRE(is_private_ipv4(0xAC1FFFFFu));  // 172.31.255.255
    // 192.168/16
    REQUIRE(is_private_ipv4(0xC0A80001u));  // 192.168.0.1
    // 127/8 回环
    REQUIRE(is_private_ipv4(0x7F000001u));  // 127.0.0.1
    // 169.254/16 链路本地
    REQUIRE(is_private_ipv4(0xA9FE0001u));  // 169.254.0.1
    // 0/8 本网络
    REQUIRE(is_private_ipv4(0x00000000u));  // 0.0.0.0
    // 100.64/10 CGNAT
    REQUIRE(is_private_ipv4(0x64400001u));  // 100.64.0.1
    // 组播 224/4
    REQUIRE(is_private_ipv4(0xE0000001u));  // 224.0.0.1
    // 保留 240/4
    REQUIRE(is_private_ipv4(0xF0000001u));  // 240.0.0.1
}

TEST_CASE("is_private_ipv4 放行公网地址", "[ssrf][ipv4]") {
    REQUIRE_FALSE(is_private_ipv4(0x08080808u));      // 8.8.8.8
    REQUIRE_FALSE(is_private_ipv4(0x01010101u));      // 1.1.1.1
    REQUIRE_FALSE(is_private_ipv4(0x5DB8D822u));      // 93.184.216.34 (example.com)
    REQUIRE_FALSE(is_private_ipv4(0xAC1FFFFFu + 1));  // 172.32.0.0（172.16/12 之外）
    REQUIRE_FALSE(is_private_ipv4(0xC0A90000u));      // 192.169.0.0（192.168/16 之外）
}

// ============================================================================
// IPv6 判定
// ============================================================================

TEST_CASE("is_private_ipv6 拦截回环/本地/组播/IPv4-mapped", "[ssrf][ipv6]") {
    // ::1 回环
    const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    REQUIRE(is_private_ipv6(loopback));
    // :: 未指定
    const uint8_t unspecified[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE(is_private_ipv6(unspecified));
    // fc00::/7 唯一本地
    const uint8_t ula[16] = {0xfd, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    REQUIRE(is_private_ipv6(ula));
    // fe80::/10 链路本地
    const uint8_t linklocal[16] = {0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    REQUIRE(is_private_ipv6(linklocal));
    // ff00::/8 组播
    const uint8_t multicast[16] = {0xff, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    REQUIRE(is_private_ipv6(multicast));
    // ::ffff:127.0.0.1 IPv4-mapped 回环
    const uint8_t mapped_loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1};
    REQUIRE(is_private_ipv6(mapped_loopback));
    // ::ffff:10.0.0.1 IPv4-mapped 内网
    const uint8_t mapped_private[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 10, 0, 0, 1};
    REQUIRE(is_private_ipv6(mapped_private));
    // 2001:db8::/32 文档
    const uint8_t doc[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    REQUIRE(is_private_ipv6(doc));
}

TEST_CASE("is_private_ipv6 放行公网地址", "[ssrf][ipv6]") {
    // 2001:4860:4860::8888 (Google DNS)
    const uint8_t pub[16] = {0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0,    0,
                             0,    0,    0,    0,    0,    0,    0x88, 0x88};
    REQUIRE_FALSE(is_private_ipv6(pub));
    // 2606:4700:4700::1111 (Cloudflare DNS)
    const uint8_t pub2[16] = {0x26, 0x06, 0x47, 0x00, 0x47, 0x00, 0,    0,
                              0,    0,    0,    0,    0,    0,    0x11, 0x11};
    REQUIRE_FALSE(is_private_ipv6(pub2));
}

// ============================================================================
// IP 字符串判定
// ============================================================================

TEST_CASE("is_private_ip_string 识别 v4/v6 字面量", "[ssrf][ipstr]") {
    REQUIRE(is_private_ip_string("127.0.0.1"));
    REQUIRE(is_private_ip_string("10.1.2.3"));
    REQUIRE(is_private_ip_string("192.168.1.1"));
    REQUIRE(is_private_ip_string("169.254.10.10"));
    REQUIRE(is_private_ip_string("::1"));
    REQUIRE(is_private_ip_string("fd00::1"));
    REQUIRE_FALSE(is_private_ip_string("8.8.8.8"));
    REQUIRE_FALSE(is_private_ip_string("2606:4700:4700::1111"));
    // 非 IP 字面量（主机名）不算内网字面量
    REQUIRE_FALSE(is_private_ip_string("example.com"));
    REQUIRE_FALSE(is_private_ip_string(""));
}

TEST_CASE("is_ip_literal 区分字面量与主机名", "[ssrf][ipstr]") {
    REQUIRE(is_ip_literal("127.0.0.1"));
    REQUIRE(is_ip_literal("8.8.8.8"));
    REQUIRE(is_ip_literal("::1"));
    REQUIRE_FALSE(is_ip_literal("example.com"));
    REQUIRE_FALSE(is_ip_literal("localhost"));
    REQUIRE_FALSE(is_ip_literal(""));
}

// ============================================================================
// 主机解析（依赖系统 DNS，仅断言确定性输入）
// ============================================================================

TEST_CASE("host_resolves_to_private 拦截回环/内网字面量", "[ssrf][host]") {
    REQUIRE(host_resolves_to_private("127.0.0.1"));
    REQUIRE(host_resolves_to_private("10.0.0.1"));
    REQUIRE(host_resolves_to_private("192.168.0.1"));
    REQUIRE(host_resolves_to_private("::1"));
    REQUIRE(host_resolves_to_private(""));
    // localhost 解析到回环
    REQUIRE(host_resolves_to_private("localhost"));
    // 公网字面量放行
    REQUIRE_FALSE(host_resolves_to_private("8.8.8.8"));
}

// ============================================================================
// HttpClient SSRF 预检（#25 P3-1）：开启防护时在 DNS/连接前拒绝内网目标
// ============================================================================

TEST_CASE("HttpClient block_private_ips 预检拒绝内网目标", "[ssrf][http]") {
    HttpClient client;
    client.set_block_private_ips(true);
    // 预检在 curl_easy_perform 之前返回，无需真实网络
    auto r = client.get("https://127.0.0.1/", {}, 1000);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::PermissionDenied);
    auto r2 = client.get("https://169.254.169.254/latest/meta-data/", {}, 1000);
    REQUIRE(r2.is_err());
    REQUIRE(r2.error().code == Error::Code::PermissionDenied);
    auto r3 = client.get("https://10.0.0.1/", {}, 1000);
    REQUIRE(r3.is_err());
    REQUIRE(r3.error().code == Error::Code::PermissionDenied);
}

TEST_CASE("HttpClient 默认不开启 SSRF 预检", "[ssrf][http]") {
    HttpClient client;  // 默认 m_block_private_ips = false
    // 不预检：内网 URL 不会因 SSRF 预检被拒（实际连接结果取决于本地环境，不断言）
    auto r = client.get("https://127.0.0.1/", {}, 500);
    if (r.is_err()) {
        REQUIRE(r.error().code != Error::Code::PermissionDenied);
    }
}
