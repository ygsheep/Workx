/**
 * @file prefix_shape.h
 * @brief 前缀形状追踪与缓存诊断（DS_CACHE_OPTIMIZATION_PLAN 层次 1）
 * @details 对 system prompt + tools schema 计算稳定 hash，每轮对比形状，
 *          当缓存命中率劣化时输出归因（system 变了 / tools 变了 / 历史被改写）。
 *          只观测，不改行为。参考 Reasonix cache_shape.go。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace agent {

/// @brief 前缀形状（system + tools 的稳定指纹）
struct PrefixShape {
    std::string system_hash;  ///< sha256(system_prompt) 前 8 字节的十六进制（L-4）
    std::string tools_hash;   ///< sha256(规范化 tools schema) 前 8 字节的十六进制
    std::string prefix_hash;  ///< system_hash + "|" + tools_hash 的联合指纹
    int rewrite_version = 0;  ///< 历史改写版本号（未来压缩集成时递增）
};

/// @brief 前缀变化诊断结果
struct PrefixChangeDiagnosis {
    std::string prefix_hash;           ///< 当前前缀形状 hash
    bool prefix_changed = false;       ///< 与上一轮相比前缀是否变化
    std::vector<std::string> reasons;  ///< 变化原因："system" / "tools" / "log_rewrite"
    int cache_hit_tokens = 0;          ///< 本轮命中 token 数（透传）
    int cache_miss_tokens = 0;         ///< 本轮未命中 token 数（透传）
};

/// @brief 捕获当前前缀形状
/// @param system_prompt 系统提示词
/// @param tools_schema 工具 schema 数组（会按 name 排序后序列化以消除顺序抖动）
/// @param rewrite_version 历史改写版本号
PrefixShape capture_shape(const std::string& system_prompt, const nlohmann::json& tools_schema,
                          int rewrite_version = 0);

/// @brief 对比前后前缀形状，输出缓存劣化归因
/// @param prev 上一轮形状（首轮传空 PrefixShape）
/// @param cur 当前形状
/// @param hit_tokens 本轮缓存命中 token 数
/// @param miss_tokens 本轮缓存未命中 token 数
/// @details 归因规则：
///          - system_hash 变化 → reasons 含 "system"
///          - tools_hash 变化 → reasons 含 "tools"
///          - rewrite_version 变化 → reasons 含 "log_rewrite"
///          - 全部未变但 miss 显著 → prefix_changed=false（非前缀因素，如缓存过期）
PrefixChangeDiagnosis compare_shape(const PrefixShape& prev, const PrefixShape& cur, int hit_tokens,
                                    int miss_tokens);

/// @brief 规范化 tools schema：按 function.name 排序后返回新数组（DS_CACHE M-2）
/// @details 消除注册顺序抖动，保证发送给 API 的 tools 字节与 hash 计算一致。
///          形如 [{"type":"function","function":{"name":"Read",...}}, ...]
/// @param tools_schema 原始 tools schema 数组
/// @return 按 name 排序后的新数组（原数组不变）
nlohmann::json normalize_tools_schema(const nlohmann::json& tools_schema);

}  // namespace agent
