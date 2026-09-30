/**
 * @file provider_config.cpp
 * @brief 供应商配置持久化实现（backend.providers 读写 + 使用中切换）
 * @details 从 workx_app/ui/provider_form.cpp 下沉：逻辑仅依赖 IConfigManager 与
 *          领域类型，供 workx（provider_form）与 codex（ftxtui 切换面板）复用。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/model/provider_config.h"

#include <nlohmann/json.hpp>

#include "agent/config/app_config.h"
#include "core/config/i_config_manager.h"

namespace agent {

std::vector<ProviderConfigEntry> load_provider_configs(IConfigManager& cfg) {
    std::vector<ProviderConfigEntry> out;
    if (!cfg.has(keys::PROVIDERS)) return out;
    auto result = cfg.get<nlohmann::json>(keys::PROVIDERS);
    if (result.is_err()) return out;
    const auto& j = result.value();
    if (!j.is_array()) return out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        ProviderConfigEntry e;
        e.id = item.value("id", "");
        e.name = item.value("name", "");
        e.base_url = item.value("base_url", "");
        e.model = item.value("model", "");
        e.context_length = item.value("context_length", 0);
        e.api_key = item.value("api_key", "");
        out.push_back(std::move(e));
    }
    return out;
}

void save_provider_configs(IConfigManager& cfg, const std::vector<ProviderConfigEntry>& providers) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& e : providers) {
        j.push_back({{"id", e.id},
                     {"name", e.name},
                     {"base_url", e.base_url},
                     {"model", e.model},
                     {"context_length", e.context_length},
                     {"api_key", e.api_key}});
    }
    cfg.set(keys::PROVIDERS, j);
}

void apply_provider_switch(IConfigManager& cfg, const ProviderConfigEntry& entry) {
    // 先清除旧键再写入新值：条目留空的字段不得残留上次供应商的配置。
    // （P1: 旧实现只 set 非空值，custom 预设全程空输入会残留旧 API_KEY/URL/MODEL_NAME）
    cfg.remove_value(keys::PROVIDER);
    cfg.remove_value(keys::REMOTE_URL);
    cfg.remove_value(keys::MODEL_NAME);
    cfg.remove_value(keys::API_KEY);

    cfg.set(keys::PROVIDER, entry.id.empty() ? entry.name : entry.id);
    if (!entry.base_url.empty()) {
        cfg.set(keys::REMOTE_URL, entry.base_url);
    }
    if (!entry.model.empty()) {
        cfg.set(keys::MODEL_NAME, entry.model);
    }
    if (!entry.api_key.empty()) {
        cfg.set(keys::API_KEY, entry.api_key);
    }
    // 上下文窗口：条目配置了显式值则写入标量（resolver 的 user cfg 级，启动/热切换均生效）；
    // 0 表示未设置，保持清除，让 catalog/静态表解析新供应商模型窗口。
    if (entry.context_length > 0) {
        cfg.set(keys::CONTEXT_LENGTH, static_cast<int>(entry.context_length));
    } else if (cfg.has(keys::CONTEXT_LENGTH)) {
        cfg.set(keys::CONTEXT_LENGTH, 0);
    }
}

}  // namespace agent