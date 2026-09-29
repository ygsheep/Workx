/**
 * @file i_config_manager.h
 * @brief 配置管理器抽象接口（C-1 DI 化）
 * @details 类型擦除的虚函数接口 + 模板包装，允许测试注入 MockConfigManager，
 *          解除对 ConfigManager 单例的硬依赖。
 *          V2-1：所有可失败 API 返回 ResultV2<T>，替代 Result<T, std::string>。
 *          M-4：按接口隔离原则（ISP）拆分为三个角色接口：
 *               - IConfigReader：只读访问（has/get_value/get_all_keys）
 *               - IConfigWriter：内存写访问（set_value）
 *               - IConfigPersistence：文件 I/O（load_from_file/save_to_file）
 *               IConfigManager 继承三者作为组合接口，向后兼容。
 *               调用方应按需依赖更窄的子接口：
 *               - 业务代码读取配置 → IConfigReader&
 *               - CLI 解析写入配置 → IConfigWriter&
 *               - 应用启动加载/保存 → IConfigPersistence&
 * @version 2.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>

#include "core/export.h"
#include <format>
#include <variant>

#include "core/utils/result_v2.h"

#include <nlohmann/json.hpp>

namespace agent {

using ConfigValue = std::variant<bool, int, double, std::string, nlohmann::json>;

/// @brief 只读配置访问接口（M-4：ISP 拆分）
/// @details 暴露配置查询能力，供业务代码读取配置值。
///          依赖此接口的代码明确表达"只读"意图，避免误调用写入方法。
class IConfigReader {
   public:
    virtual ~IConfigReader() = default;

    /// @brief 检查配置键是否存在
    [[nodiscard]] virtual bool has(const std::string& key) const = 0;

    /// @brief 获取配置值（类型擦除）
    /// @return 成功返回 ConfigValue；失败返回 Error（ConfigMissing）
    [[nodiscard]] virtual ResultV2<ConfigValue> get_value(const std::string& key) const = 0;

    /// @brief 获取所有配置键
    [[nodiscard]] virtual std::vector<std::string> get_all_keys() const = 0;

    // === 模板包装（非虚，委托 get_value）===

    /// @brief 获取配置值（类型安全）
    /// @return 成功返回 T；失败返回 Error（ConfigMissing 或 ConfigInvalid）
    template <typename T>
    [[nodiscard]] ResultV2<T> get(const std::string& key) const {
        auto result = get_value(key);
        if (result.is_err()) {
            return result.error();
        }
        const auto& v = result.value();
        if (!std::holds_alternative<T>(v)) {
            return ResultV2<T>::err(Error::Code::ConfigInvalid,
                                    std::format("Type mismatch for config key '{}'", key), key);
        }
        return ResultV2<T>::ok(std::get<T>(v));
    }

    /// @brief 获取配置值，失败时返回默认值
    template <typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        auto result = get<T>(key);
        return result.is_ok() ? std::move(result.value()) : std::move(default_value);
    }
};

/// @brief 内存写入配置接口（M-4：ISP 拆分）
/// @details 暴露配置内存写能力，供 CLI 解析、运行时配置修改等场景使用。
///          与 IConfigPersistence 分离后，CLI 解析层可只依赖此接口，
///          表达"只写内存、不持久化"的语义。
class IConfigWriter {
   public:
    virtual ~IConfigWriter() = default;

    /// @brief 设置配置值（类型擦除）
    /// @return 成功返回 void；失败返回 Error（ConfigInvalid）
    virtual ResultV2<void> set_value(const std::string& key, ConfigValue value) = 0;

    /// @brief 删除配置键（key 不存在时视为成功）
    /// @return 成功返回 void
    virtual ResultV2<void> remove_value(const std::string& key) = 0;

    // === 模板包装（非虚，委托 set_value）===

    /// @brief 设置配置值（类型安全）
    template <typename T>
    ResultV2<void> set(const std::string& key, T value) {
        return set_value(key, ConfigValue(std::move(value)));
    }
};

/// @brief 配置持久化接口（M-4：ISP 拆分）
/// @details 暴露文件 I/O 能力，供应用启动加载配置、退出保存配置等场景使用。
///          与内存读写接口分离后，纯业务逻辑（如工厂构造、工具执行）无需
///          依赖文件 I/O 类型，降低耦合面。
class IConfigPersistence {
   public:
    virtual ~IConfigPersistence() = default;

    /// @brief 从文件加载配置
    /// @return 成功返回 void；失败返回 Error（ResourceNotFound/ConfigParseFailed）
    virtual ResultV2<void> load_from_file(const std::filesystem::path& path) = 0;

    /// @brief 保存配置到文件
    /// @return 成功返回 void；失败返回 Error（ConfigParseFailed）
    virtual ResultV2<void> save_to_file(const std::filesystem::path& path) = 0;
};

/// @brief 配置管理器组合接口（M-4：组合 IConfigReader/IConfigWriter/IConfigPersistence）
/// @details M-4：组合三个角色接口，保留 IConfigManager 作为实现类的统一入口。
///          ConfigManager 实现 IConfigManager 即可获得三个角色；新代码应按需依赖
///          更窄的子接口（如业务读取代码只依赖 IConfigReader&）。
///
/// 历史说明：
/// - 类型擦除的虚函数接口供 DI 注入
/// - 模板包装方法委托虚函数，调用方可像使用 ConfigManager 一样使用 IConfigManager&
class WORKX_API IConfigManager : public IConfigReader,
                                 public IConfigWriter,
                                 public IConfigPersistence {
    // 纯抽象组合。模板包装方法已分别移至 IConfigReader / IConfigWriter。
    // 保留 IConfigManager 类型供现有代码作为 DI 注入点，无需改动。
};

}  // namespace agent
