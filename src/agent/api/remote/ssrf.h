/**
 * @file ssrf.h
 * @brief SSRF 防护原语（IP 级内网/回环/链路本地地址判定）
 * @details 供 HttpClient 的 CURLOPT_OPENSOCKETFUNCTION 连接钩子与
 *          WebFetchTool 的 URL 预检共用，避免两处重复实现。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <string>

namespace agent {

/// @brief 判断 IPv4（主机字节序）是否为内网/回环/链路本地/保留地址
/// @details 覆盖 0/8、10/8、100.64/10(CGNAT)、127/8、169.254/16、
///          172.16/12、192.168/16、224/4(组播)、240/4(保留)
bool is_private_ipv4(uint32_t ip_host_order) noexcept;

/// @brief 判断 IPv6（16 字节网络字节序）是否为内网/回环/链路本地/组播地址
/// @details 覆盖 ::、::1、fc00::/7、fe80::/10、ff00::/8、
///          ::ffff:0:0/96(IPv4-mapped，内嵌 v4 再判)、2001:db8::/32(文档)
bool is_private_ipv6(const uint8_t addr[16]) noexcept;

/// @brief 判断 IP 字符串（IPv4/IPv6 字面量）是否为内网地址
/// @return true = 内网/回环/链路本地；非 IP 字面量（主机名）返回 false
bool is_private_ip_string(const std::string& ip) noexcept;

/// @brief 判断字符串是否为 IP 字面量（IPv4 或 IPv6）
bool is_ip_literal(const std::string& s) noexcept;

/// @brief 解析主机名并判断是否有任一解析结果落在内网
/// @param host 主机名或 IP 字面量
/// @return true = 不安全（解析失败、或任一结果命中内网地址）
bool host_resolves_to_private(const std::string& host) noexcept;

}  // namespace agent
