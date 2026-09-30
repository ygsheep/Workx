/**
 * @file model_catalog.h
 * @brief models.dev 模型目录（远程 catalog）
 * @details 从 https://models.dev/api.json 拉取模型信息，本地缓存于
 *          <config_dir>/models_cache.json，作为 resolve_context_length 的
 *          补充数据源（优先于内置静态表，不阻塞启动）。
 *          启动时先加载缓存（离线可用）→ 后台线程拉取 → 成功写缓存 + 更新内存。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include "core/utils/result_v2.h"

namespace agent {

/// @brief models.dev 模型目录
/// @details 线程安全的目录：仅替换整个实例（std::shared_ptr），内部不提供修改方法。
///          匹配规则（与 find_model_capability 对齐）：
///            1. 精确匹配（不区分大小写）
///            2. 去 provider 前缀匹配（如 "kimi/kimi-k3" → "kimi-k3"）
///            3. 最长子串匹配
class ModelCatalog {
   public:
    /// @brief 模型能力信息
    struct ModelInfo {
        int32_t context_window = 0;     ///< 上下文窗口 token 数
        int32_t max_output_tokens = 0;  ///< 最大输出 token 数
    };

    ModelCatalog() = default;

    /// @brief 从 models.dev api.json 内容构建目录
    /// @details 宽容解析：provider 或 model 条目无效时跳过，不整体失败。
    ///          格式：{ providerId: { models: { modelId: { limit: { context, output } } } } }
    ///          合并策略：同名模型在多个 provider 下出现时，官方直营 provider
    ///          （deepseek/zhipuai/moonshotai/alibaba/minimax/anthropic/openai 等）的值优先，
    ///          其余 provider 仅补充官方未收录的模型（first-wins）。
    /// @param json_text api.json 原始内容
    /// @return 解析成功返回目录；JSON 解析失败返回 Error
    static ResultV2<ModelCatalog> from_api_json(std::string_view json_text);

    /// @brief 从本地缓存文件加载
    /// @details 缓存文件是 from_api_json 解析结果的扁平化保存（{ model_id: {context, output} }）
    /// @return 加载成功返回目录；文件不存在或解析失败返回 Error
    static ResultV2<ModelCatalog> load_cache(const std::filesystem::path& path);

    /// @brief 保存为本地缓存文件（扁平化格式）
    /// @details 父目录不存在时自动创建
    ResultV2<void> save_cache(const std::filesystem::path& path) const;

    /// @brief 查询模型 context window
    /// @details 未命中返回 0
    int32_t context_window_for(std::string_view model_name) const;

    /// @brief 查询模型最大输出 token
    /// @details 未命中返回 0
    int32_t max_output_tokens_for(std::string_view model_name) const;

    /// @brief 是否包含精确匹配的模型
    bool contains(std::string_view model_name) const;

    /// @brief 目录是否为空（无任何模型）
    bool empty() const { return m_models.empty(); }

    /// @brief 模型数量
    std::size_t size() const { return m_models.size(); }

   private:
    /// @brief 内部查找：返回模型信息指针，未命中返回 nullptr
    const ModelInfo* find(std::string_view model_name) const;

    std::unordered_map<std::string, ModelInfo> m_models;  ///< key = 规范化模型名（小写）
};

}  // namespace agent
