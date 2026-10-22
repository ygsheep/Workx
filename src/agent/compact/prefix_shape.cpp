/**
 * @file prefix_shape.cpp
 * @brief 前缀形状追踪与缓存诊断实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "prefix_shape.h"
#include "agent/compact/sha256.h"

#include <algorithm>
#include <cstdint>

namespace agent {

/// @brief 规范化 tools schema：按 function.name 排序后返回新数组（公开函数，DS_CACHE M-2）
/// @details 同时用于 capture_shape 的 hash 计算和 build_request 的 tools 排序，
///          保证两者字节级一致。返回排序后的新数组，原数组不变。
nlohmann::json normalize_tools_schema(const nlohmann::json& tools_schema) {
    if (tools_schema.is_null() || !tools_schema.is_array() || tools_schema.empty()) {
        return nlohmann::json::array();
    }

    // 提取 (name, tool_json) 对
    std::vector<std::pair<std::string, nlohmann::json>> entries;
    entries.reserve(tools_schema.size());
    for (const auto& tool : tools_schema) {
        std::string name;
        if (tool.contains("function") && tool["function"].contains("name")) {
            name = tool["function"]["name"].get<std::string>();
        } else if (tool.contains("name")) {
            name = tool["name"].get<std::string>();
        }
        entries.emplace_back(name, tool);
    }

    // 按 name 排序
    std::sort(entries.begin(), entries.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    // 构建排序后的新数组
    nlohmann::json result = nlohmann::json::array();
    for (auto& [name, tool] : entries) {
        result.push_back(std::move(tool));
    }
    return result;
}

PrefixShape capture_shape(const std::string& system_prompt, const nlohmann::json& tools_schema,
                          int rewrite_version) {
    // L-4：使用 sha256 替代 std::hash
    // - 跨编译器/跨进程确定（std::hash 实现定义，MSVC/GCC 结果不同）
    // - 可外部验证（Python hashlib.sha256 / openssl dgst -sha256 复算）
    // - 对齐 Plan 第 181 行 "sha256 前 8 字节" 要求
    PrefixShape shape;
    shape.system_hash = compact::sha256_hex(system_prompt, 8);
    // M-2：normalize_tools_schema 现在返回 json 数组，序列化后再 hash
    auto normalized = normalize_tools_schema(tools_schema);
    shape.tools_hash = compact::sha256_hex(normalized.dump(), 8);
    shape.prefix_hash = shape.system_hash + "|" + shape.tools_hash;
    shape.rewrite_version = rewrite_version;
    return shape;
}

PrefixChangeDiagnosis compare_shape(const PrefixShape& prev, const PrefixShape& cur, int hit_tokens,
                                    int miss_tokens) {
    PrefixChangeDiagnosis diag;
    diag.prefix_hash = cur.prefix_hash;
    diag.cache_hit_tokens = hit_tokens;
    diag.cache_miss_tokens = miss_tokens;

    // 首轮（prev.prefix_hash 为空）不诊断为变化
    if (prev.prefix_hash.empty()) {
        return diag;
    }

    std::vector<std::string> reasons;

    if (prev.system_hash != cur.system_hash) {
        reasons.push_back("system");
    }
    if (prev.tools_hash != cur.tools_hash) {
        reasons.push_back("tools");
    }
    if (prev.rewrite_version != cur.rewrite_version) {
        reasons.push_back("log_rewrite");
    }

    diag.prefix_changed = !reasons.empty();
    diag.reasons = std::move(reasons);
    return diag;
}

}  // namespace agent
