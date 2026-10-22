/**
 * @file provider_config.h
 * @brief 供应商配置领域类型（ProviderConfigEntry / ProviderSwitchResult）
 * @details 与持久化 JSON 字段一一对应（backend.providers 数组条目）。
 *          独立于 UI 层（provider_form），供 tui 向导等不依赖 app 的组件引用。
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace agent {

class IConfigManager;

/// @brief 供应商配置条目（与持久化 JSON 字段一一对应）
struct ProviderConfigEntry {
    std::string id;    ///< 标识（预设内部名或自定义名称，写入 backend.provider）
    std::string name;  ///< 显示名
    std::string base_url;        ///< API 基础 URL
    std::string model;           ///< 模型 ID
    int32_t context_length = 0;  ///< 上下文窗口（token），0 = 未知
    std::string api_key;         ///< API Key
};

/// @brief 供应商面板返回结果
struct ProviderSwitchResult {
    bool applied = false;       ///< 是否执行了"设为使用中"（需要热切换）
    ProviderConfigEntry entry;  ///< 切换到的条目（applied=true 时有效）
};

/// @brief 从配置读取供应商列表（backend.providers JSON 数组）
std::vector<ProviderConfigEntry> load_provider_configs(IConfigManager& cfg);

/// @brief 保存供应商列表到配置（backend.providers）
void save_provider_configs(IConfigManager& cfg, const std::vector<ProviderConfigEntry>& providers);

/// @brief 将条目设为使用中（写 backend.provider / remote_url / model / api_key）
/// @details 先清除旧标量键再写入新值，防止残留上次供应商配置。
void apply_provider_switch(IConfigManager& cfg, const ProviderConfigEntry& entry);

}  // namespace agent
