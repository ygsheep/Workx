/**
 * @file registry.h
 * @brief ToolRegistry — 工具注册表
 * @details 管理所有可用工具的生命周期：注册、查找、列举、schema 生成
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>

#include "core/export.h"
#include <unordered_map>
#include <nlohmann/json.hpp>
#include "itool.h"

namespace agent::tool {

/// @brief ToolRegistry — 工具注册表
///
/// 管理所有可用工具的生命周期：
/// - 注册/注销工具实例
/// - 按名称查找工具
/// - 列举所有工具的 schema（供 LLM function calling 使用）
class WORKX_API ToolRegistry {
   public:
    /// @brief 注册工具
    /// @param tool 工具实例
    inline void register_tool(std::shared_ptr<ITool> tool) {
        if (!tool) return;
        name_index_[tool->name()] = tool;
        tools_.push_back(std::move(tool));
    }

    /// @brief 按名称查找工具
    /// @param name 工具名称
    /// @return 工具实例（未找到返回 nullptr）
    inline std::shared_ptr<ITool> find_by_name(const std::string& name) const {
        auto it = name_index_.find(name);
        return it != name_index_.end() ? it->second : nullptr;
    }

    /// @brief 获取所有已注册工具
    /// @return 工具列表
    inline std::vector<std::shared_ptr<ITool>> get_all_tools() const { return tools_; }

    /// @brief 获取所有工具的 schema（供 LLM function calling）
    /// @return JSON 数组，每个元素包含 name/description/input_schema
    inline nlohmann::json get_all_schemas() const {
        nlohmann::json schemas = nlohmann::json::array();
        for (const auto& tool : tools_) {
            schemas.push_back({
                {"name", tool->name()},
                {"description", tool->description()},
                {"input_schema", tool->input_schema()},
            });
        }
        return schemas;
    }

    /// @brief 按名称白名单获取 schema（供子 Agent 工具集过滤）
    /// @param names 允许的工具名称列表；仅保留名称匹配且已注册的工具
    /// @return JSON 数组，每个元素包含 name/description/input_schema
    inline nlohmann::json get_schemas_by_names(const std::vector<std::string>& names) const {
        nlohmann::json schemas = nlohmann::json::array();
        for (const auto& name : names) {
            auto it = name_index_.find(name);
            if (it == name_index_.end()) continue;  // 未注册的工具名忽略
            const auto& tool = it->second;
            schemas.push_back({
                {"name", tool->name()},
                {"description", tool->description()},
                {"input_schema", tool->input_schema()},
            });
        }
        return schemas;
    }

    /// @brief 检查工具是否存在
    /// @param name 工具名称
    /// @return 存在返回 true
    inline bool exists(const std::string& name) const { return name_index_.contains(name); }

    /// @brief 获取工具数量
    /// @return 工具数量
    inline size_t size() const { return tools_.size(); }

   private:
    std::vector<std::shared_ptr<ITool>> tools_;
    std::unordered_map<std::string, std::shared_ptr<ITool>> name_index_;
};

}  // namespace agent::tool
