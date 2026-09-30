/**
 * @file config_manager.h
 * @brief 配置管理器
 * @details 类型安全、分层配置、验证回调、JSON 持久化。
 *          继承 IConfigManager 支持 DI 注入（C-1）。
 * @version 2.0.0
 */

#pragma once

#include "core/utils/result.h"     // 旧 Result（过渡期保留，V2-8 标记 deprecated）
#include "core/utils/result_v2.h"  // V2-1：新 ResultV2
#include "core/config/i_config_manager.h"
#include "core/export.h"
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace agent {

/// @brief 配置元数据（旧版，向后兼容）
/// @details V2-1：validate_callback 签名改为返回 ResultV2<void>
struct ConfigMeta {
    std::string description;
    ConfigValue default_value;
    bool is_required = false;

    using ValidateCallback = std::function<ResultV2<void>(const ConfigValue&)>;
    ValidateCallback validate_callback;

    using ChangeCallback = std::function<void(const ConfigValue&)>;
    ChangeCallback change_callback;
};

/// @brief 结构化配置 Schema（C-2/C-4）
/// @details 在 ConfigMeta 基础上增加类型约束、范围约束、枚举值、环境变量映射。
///          register_schema() 注册后，set_value() 时自动校验。
/// @note 所有字段均提供默认成员初始化器，聚合初始化时省略字段不会触发
///       GCC `-Wmissing-field-initializers` 警告。
struct ConfigSchema {
    std::string key;            ///< 配置键
    std::string description;    ///< 人类可读描述
    ConfigValue default_value;  ///< 默认值
    bool is_required = false;   ///< 是否必填

    /// @brief 类型约束
    enum class Type : int {
        Bool,    ///< 布尔
        Int,     ///< 整数
        Double,  ///< 浮点
        String,  ///< 字符串
        Enum     ///< 枚举（enum_values 限定）
    } type = Type::String;

    /// @brief 整数范围约束（仅 type=Int 时生效）
    std::optional<std::pair<int64_t, int64_t>> int_range = std::nullopt;
    /// @brief 浮点范围约束（仅 type=Double 时生效）
    std::optional<std::pair<double, double>> double_range = std::nullopt;
    /// @brief 枚举值列表（仅 type=Enum 时生效）
    std::vector<std::string> enum_values = {};
    /// @brief 对应的环境变量名（C-4，空表示不绑定环境变量）
    std::string env_var = {};
    /// @brief 自定义验证函数（与类型/范围/枚举校验叠加，均需通过）
    /// @details V2-1：返回 ResultV2<void>，错误码应为 ConfigInvalid
    std::function<ResultV2<void>(const ConfigValue&)> validate = {};

    /// @brief 校验配置值是否符合 schema
    /// @param value 待校验的值
    /// @return 成功返回 ok；失败返回 Error（ConfigInvalid）
    [[nodiscard]] ResultV2<void> validate_value(const ConfigValue& value) const;
};

class WORKX_API ConfigManager final : public IConfigManager {
   public:
    static ConfigManager& instance() noexcept {
        static ConfigManager inst;
        return inst;
    }

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    // === IConfigManager 类型擦除接口实现（V2-1：返回 ResultV2）===

    [[nodiscard]] bool has(const std::string& key) const override;

    [[nodiscard]] ResultV2<ConfigValue> get_value(const std::string& key) const override;

    ResultV2<void> set_value(const std::string& key, ConfigValue value) override;

    ResultV2<void> load_from_file(const std::filesystem::path& path) override;
    ResultV2<void> save_to_file(const std::filesystem::path& path) override;

    [[nodiscard]] std::vector<std::string> get_all_keys() const override;

    // === 保留的 template 方法（与基类模板包装功能相同，供已存在调用方使用）===
    // 注意：基类 IConfigManager 已提供 set/get/get_or 模板包装，这里不再重复声明，
    //       调用方通过 IConfigManager& 接口即可使用。下方 register_meta 等
    //       非模板扩展方法仍保留为 ConfigManager 专属。

    void remove(const std::string& key);
    ResultV2<void> remove_value(const std::string& key) override;
    void register_meta(const std::string& key, ConfigMeta meta);
    [[nodiscard]] ResultV2<ConfigMeta> get_meta(const std::string& key) const;

    // === C-2/C-4：结构化 Schema ===

    /// @brief 注册配置 Schema
    /// @details 注册后 set_value() 时自动校验类型/范围/枚举/自定义验证
    void register_schema(ConfigSchema schema);

    /// @brief 获取配置 Schema
    /// @return 成功返回 Schema；失败返回 Error（ConfigMissing）
    [[nodiscard]] ResultV2<ConfigSchema> get_schema(const std::string& key) const;

    /// @brief 获取所有已注册 Schema
    [[nodiscard]] std::vector<ConfigSchema> get_all_schemas() const;

    /// @brief 从环境变量加载配置（C-4）
    /// @details 遍历所有已注册 Schema，若 env_var 非空且对应环境变量存在，
    ///          按 schema.type 解析值并 set_value。默认值也会被应用。
    void load_from_env();

    void add_change_callback(
        std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)> callback);
    void clear_change_callbacks();
    void clear();

   private:
    ConfigManager() = default;
    ~ConfigManager() override = default;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ConfigValue> m_values;
    std::unordered_map<std::string, ConfigMeta> m_metas;
    std::unordered_map<std::string, ConfigSchema> m_schemas;  ///< C-2 结构化 Schema
    std::vector<std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)>>
        m_global_change_callbacks;
};

class ConfigScope {
   public:
    /// @brief 构造 ConfigScope
    /// @param prefix 键前缀（如 "backend."）
    /// @param reader 只读配置访问接口（M-4 ISP：ConfigScope 仅需读能力）
    /// @param writer 内存写入接口（M-4 ISP：ConfigScope 仅需内存写能力，不需持久化）
    /// @note H-A：原签名接收 IConfigManager&（胖接口），现拆为 IConfigReader& + IConfigWriter&，
    ///       明确 ConfigScope 不依赖 IConfigPersistence（无法 save_to_file / load_from_file）。
    ///       ConfigManager 同时实现三者，调用方仍可传同一对象。
    explicit ConfigScope(const std::string& prefix, IConfigReader& reader, IConfigWriter& writer);
    ~ConfigScope();

    ConfigScope(const ConfigScope&) = delete;
    ConfigScope& operator=(const ConfigScope&) = delete;

    [[nodiscard]] std::string make_key(const std::string& key) const;

    /// @brief 获取底层配置读取器引用（M-4：暴露窄接口，供需要只读访问的场景使用）
    [[nodiscard]] IConfigReader& config_reader() const { return m_reader.get(); }
    /// @brief 获取底层配置写入器引用（M-4：暴露窄接口，供需要内存写的场景使用）
    [[nodiscard]] IConfigWriter& config_writer() const { return m_writer.get(); }

    template <typename T>
    ResultV2<void> set(const std::string& key, T value) {
        return m_writer.get().set(make_key(key), std::move(value));
    }

    template <typename T>
    [[nodiscard]] ResultV2<T> get(const std::string& key) const {
        return m_reader.get().get<T>(make_key(key));
    }

    template <typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        return m_reader.get().get_or(make_key(key), default_value);
    }

   private:
    std::string m_prefix;
    std::reference_wrapper<IConfigReader> m_reader;  // M-4 ISP：只读访问
    std::reference_wrapper<IConfigWriter> m_writer;  // M-4 ISP：内存写访问
};

}  // namespace agent
