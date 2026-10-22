/**
 * @file ssrf.cpp
 * @brief SSRF 防护原语实现（纯函数，可单测）
 */

#include "agent/api/remote/ssrf.h"

#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#endif

namespace agent {

bool is_private_ipv4(uint32_t ip) noexcept {
    // 0/8 本网络、127/8 回环
    if (ip < 0x01000000u) return true;                       // 0.0.0.0 - 0.255.255.255
    if (ip >= 0x7f000000u && ip <= 0x7fffffff) return true;  // 127.0.0.0/8
    // 10/8
    if (ip >= 0x0a000000u && ip <= 0x0affffff) return true;
    // 100.64/10 CGNAT
    if (ip >= 0x64400000u && ip <= 0x647fffff) return true;
    // 169.254/16 链路本地
    if (ip >= 0xa9fe0000u && ip <= 0xa9feffff) return true;
    // 172.16/12
    if (ip >= 0xac100000u && ip <= 0xac1fffff) return true;
    // 192.168/16
    if (ip >= 0xc0a80000u && ip <= 0xc0a8ffff) return true;
    // 224/4 组播、240/4 保留
    if (ip >= 0xe0000000u) return true;
    return false;
}

bool is_private_ipv6(const uint8_t a[16]) noexcept {
    // ::
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (a[i]) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return true;
    // ::1 回环
    static const uint8_t kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (std::memcmp(a, kLoopback, 16) == 0) return true;
    // fe80::/10 链路本地
    if (a[0] == 0xfe && (a[1] & 0xc0) == 0x80) return true;
    // fc00::/7 唯一本地
    if ((a[0] & 0xfe) == 0xfc) return true;
    // ff00::/8 组播
    if (a[0] == 0xff) return true;
    // 2001:db8::/32 文档
    if (a[0] == 0x20 && a[1] == 0x01 && a[2] == 0x0d && a[3] == 0xb8) return true;
    // ::ffff:0:0/96 IPv4-mapped —— 内嵌 v4 再判
    static const uint8_t kPrefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::memcmp(a, kPrefix, 12) == 0) {
        uint32_t v4 = (static_cast<uint32_t>(a[12]) << 24) | (static_cast<uint32_t>(a[13]) << 16) |
                      (static_cast<uint32_t>(a[14]) << 8) | static_cast<uint32_t>(a[15]);
        return is_private_ipv4(v4);
    }
    return false;
}

bool is_ip_literal(const std::string& s) noexcept {
    if (s.empty()) return false;
    struct in_addr a4;
    if (inet_pton(AF_INET, s.c_str(), &a4) == 1) return true;
    struct in6_addr a6;
    if (inet_pton(AF_INET6, s.c_str(), &a6) == 1) return true;
    return false;
}

bool is_private_ip_string(const std::string& ip) noexcept {
    if (ip.empty()) return false;
    struct in_addr a4;
    if (inet_pton(AF_INET, ip.c_str(), &a4) == 1) {
        return is_private_ipv4(ntohl(a4.s_addr));
    }
    struct in6_addr a6;
    if (inet_pton(AF_INET6, ip.c_str(), &a6) == 1) {
        return is_private_ipv6(a6.s6_addr);
    }
    return false;
}

bool host_resolves_to_private(const std::string& host) noexcept {
    if (host.empty()) return true;
    // IP 字面量直接判，避免 getaddrinfo 对畸形字面量的歧义
    if (is_ip_literal(host)) return is_private_ip_string(host);

    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0) {
        return true;  // 解析失败视为不安全（fail-closed）
    }
    bool unsafe = false;
    for (auto* p = res; p && !unsafe; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            auto* a = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
            if (is_private_ipv4(ntohl(a->sin_addr.s_addr))) unsafe = true;
        } else if (p->ai_family == AF_INET6) {
            auto* a = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
            if (is_private_ipv6(a->sin6_addr.s6_addr)) unsafe = true;
        }
    }
    freeaddrinfo(res);
    return unsafe;
}

}  // namespace agent
