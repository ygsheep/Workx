/**
 * @file pricing_table.cpp
 * @brief 模型单价表实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "island/pricing_table.h"

namespace island {

PricingTable PricingTable::deepseek_default() {
    PricingTable table;
    table.m_models = {
        {.model = "deepseek-v4-flash",
         .input_per_1m = 0.27,
         .output_per_1m = 1.10,
         .cache_read_per_1m = 0.07,
         .cache_write_per_1m = 0.27,
         .context_window = 1000000},
        {.model = "deepseek-v4-reasoner",
         .input_per_1m = 0.55,
         .output_per_1m = 2.19,
         .cache_read_per_1m = 0.14,
         .cache_write_per_1m = 0.55,
         .context_window = 1000000},
        {.model = "deepseek-chat",
         .input_per_1m = 0.27,
         .output_per_1m = 1.10,
         .cache_read_per_1m = 0.07,
         .cache_write_per_1m = 0.27,
         .context_window = 64000},
    };
    return table;
}

agent::ResultV2<PricingTable> PricingTable::load_from_json(const std::string& json_text) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception& e) {
        return agent::Error(agent::Error::Code::InvalidFormat,
                            std::string("pricing json 解析失败: ") + e.what());
    }
    if (!j.is_array()) {
        return agent::Error(agent::Error::Code::InvalidFormat, "pricing json 顶层必须是数组");
    }
    if (j.empty()) {
        return agent::Error(agent::Error::Code::InvalidFormat, "pricing 数组不能为空");
    }

    PricingTable table;
    for (const auto& item : j) {
        if (!item.is_object()) {
            return agent::Error(agent::Error::Code::InvalidFormat, "pricing 数组元素必须是对象");
        }
        const std::string model = item.value("model", "");
        if (model.empty()) {
            return agent::Error(agent::Error::Code::InvalidFormat, "pricing 条目缺少 model 字段");
        }
        ModelPricing p;
        p.model = model;
        p.input_per_1m = item.value("input_per_1m", 0.0);
        p.output_per_1m = item.value("output_per_1m", 0.0);
        p.cache_read_per_1m = item.value("cache_read_per_1m", 0.0);
        p.cache_write_per_1m = item.value("cache_write_per_1m", 0.0);
        p.context_window = item.value("context_window", 0);
        table.m_models.push_back(std::move(p));
    }
    return agent::ResultV2<PricingTable>::ok(std::move(table));
}

const ModelPricing* PricingTable::find(const std::string& model) const noexcept {
    for (const auto& p : m_models) {
        if (p.model == model) return &p;
    }
    return nullptr;
}

nlohmann::json PricingTable::to_json() const {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& p : m_models) {
        arr.push_back({
            {"model", p.model},
            {"input_per_1m", p.input_per_1m},
            {"output_per_1m", p.output_per_1m},
            {"cache_read_per_1m", p.cache_read_per_1m},
            {"cache_write_per_1m", p.cache_write_per_1m},
            {"context_window", p.context_window},
        });
    }
    return arr;
}

}  // namespace island