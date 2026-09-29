/**
 * @file pricing_table.h
 * @brief 模型单价表（DeepSeek 2026-04 官方定价 fallback + 用户 JSON 覆盖）
 * @details 加载顺序：
 *          1. default_deepseek() 硬编码 fallback
 *          2. load_from_json() 用户文件（~/.workx/pricing.json）覆盖
 *          3. GUI 首次连接时通过 get_model_pricing 请求拉取本表
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/utils/result_v2.h"

namespace island {

/// @brief 单个模型单价（USD / 1M tokens）
struct ModelPricing {
    std::string model;
    double input_per_1m = 0.0;        ///< 普通输入（cache miss）
    double output_per_1m = 0.0;       ///< 输出
    double cache_read_per_1m = 0.0;   ///< 缓存命中读
    double cache_write_per_1m = 0.0;  ///< 缓存写入（DeepSeek 同输入单价）
    int context_window = 0;           ///< 上下文窗口（token）
};

/// @brief 单价表
class PricingTable {
   public:
    /// @brief 构建 DeepSeek 官方定价 fallback 表
    static PricingTable deepseek_default();

    /// @brief 从用户 JSON 加载覆盖表
    /// @param json_text 数组格式：[{model, input_per_1m, output_per_1m,
    ///        cache_read_per_1m, cache_write_per_1m, context_window}]
    /// @return 解析失败返回 Error（InvalidFormat，含具体字段名）
    static agent::ResultV2<PricingTable> load_from_json(const std::string& json_text);

    /// @brief 查找模型单价（精确匹配）
    /// @return 未找到返回 nullptr
    [[nodiscard]] const ModelPricing* find(const std::string& model) const noexcept;

    /// @brief 全部模型列表
    [[nodiscard]] const std::vector<ModelPricing>& all() const noexcept { return m_models; }

    /// @brief 序列化为 JSON 数组（get_model_pricing 响应）
    [[nodiscard]] nlohmann::json to_json() const;

   private:
    std::vector<ModelPricing> m_models;
};

}  // namespace island