/**
 * @file config_manager.cpp
 * @brief 配置管理器实现
 */

#include "core/config/config_manager.h"
#include "core/utils/file_permissions.h"
#include "liblogger/logger.h"

#include <format>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace agent {

namespace {

/// 将 dot 分隔的 key 写入嵌套 JSON（如 "backend.api_key" → j["backend"]["api_key"]）
void set_nested_json(nlohmann::json& j, const std::string& key, const ConfigValue& value) {
    nlohmann::json* current = &j;
    size_t start = 0;
    while (true) {
        size_t dot = key.find('.', start);
        if (dot == std::string::npos) {
            std::string leaf = key.substr(start);
            if (std::holds_alternative<bool>(value)) {
                (*current)[leaf] = std::get<bool>(value);
            } else if (std::holds_alternative<int>(value)) {
                (*current)[leaf] = std::get<int>(value);
            } else if (std::holds_alternative<double>(value)) {
                (*current)[leaf] = std::get<double>(value);
            } else if (std::holds_alternative<std::string>(value)) {
                (*current)[leaf] = std::get<std::string>(value);
            } else if (std::holds_alternative<nlohmann::json>(value)) {
                (*current)[leaf] = std::get<nlohmann::json>(value);
            }
            return;
        }
        std::string section = key.substr(start, dot - start);
        if (!current->contains(section) || !(*current)[section].is_object()) {
            (*current)[section] = nlohmann::json::object();
        }
        current = &(*current)[section];
        start = dot + 1;
    }
}

/// 递归展平嵌套 JSON object 为 dot 分隔的 flat key
void flatten_json(const nlohmann::json& j, const std::string& prefix,
                  std::unordered_map<std::string, ConfigValue>& values) {
    for (auto& [key, value] : j.items()) {
        std::string full_key = prefix.empty() ? key : prefix + "." + key;
        if (value.is_boolean()) {
            values[full_key] = value.get<bool>();
        } else if (value.is_number_integer()) {
            values[full_key] = value.get<int>();
        } else if (value.is_number()) {
            values[full_key] = value.get<double>();
        } else if (value.is_string()) {
            values[full_key] = value.get<std::string>();
        } else if (value.is_array()) {
            // 数组整体保存为 JSON 值（如 backend.providers 多供应商列表）
            values[full_key] = value;
        } else if (value.is_object()) {
            flatten_json(value, full_key, values);
        }
        // null 被忽略
    }
}

}  // anonymous namespace

bool ConfigManager::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_values.find(key) != m_values.end();
}

void ConfigManager::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_values.erase(key);
}

ResultV2<void> ConfigManager::remove_value(const std::string& key) {
    remove(key);
    return ResultV2<void>::ok();
}

void ConfigManager::register_meta(const std::string& key, ConfigMeta meta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metas[key] = std::move(meta);
}

ResultV2<ConfigMeta> ConfigManager::get_meta(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_metas.find(key);
    if (it == m_metas.end()) {
        return ResultV2<ConfigMeta>::err(Error::Code::ConfigMissing,
                                         std::format("Config meta '{}' not found", key), key);
    }
    return ResultV2<ConfigMeta>::ok(it->second);
}

// === IConfigManager 类型擦除接口实现（V2-1：返回 ResultV2）===

ResultV2<ConfigValue> ConfigManager::get_value(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_values.find(key);
    if (it == m_values.end()) {
        // 尝试 meta 的默认值
        auto meta_it = m_metas.find(key);
        if (meta_it != m_metas.end()) {
            return ResultV2<ConfigValue>::ok(meta_it->second.default_value);
        }
        return ResultV2<ConfigValue>::err(Error::Code::ConfigMissing,
                                          std::format("Config key '{}' not found", key), key);
    }
    return ResultV2<ConfigValue>::ok(it->second);
}

ResultV2<void> ConfigManager::set_value(const std::string& key, ConfigValue config_value) {
    // C-2：Schema 校验（优先于 meta.validate_callback）
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto schema_it = m_schemas.find(key);
        if (schema_it != m_schemas.end()) {
            auto result = schema_it->second.validate_value(config_value);
            if (result.is_err()) {
                return ResultV2<void>::err(Error::Code::ConfigInvalid,
                                           std::format("Schema validation failed for '{}': {}", key,
                                                       result.error().message),
                                           key);
            }
        }
    }

    auto meta_it = m_metas.end();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        meta_it = m_metas.find(key);
    }

    if (meta_it != m_metas.end()) {
        auto& meta = meta_it->second;
        if (meta.validate_callback) {
            auto result = meta.validate_callback(config_value);
            if (result.is_err()) {
                return ResultV2<void>::err(
                    Error::Code::ConfigInvalid,
                    std::format("Validation failed for '{}': {}", key, result.error().message),
                    key);
            }
        }
    }

    ConfigValue old_value;
    bool has_old_value = false;
    ConfigMeta::ChangeCallback change_callback;
    bool has_change_callback = false;
    std::vector<std::function<void()>> pending_callbacks;

    {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto old_value_it = m_values.find(key);
        has_old_value = (old_value_it != m_values.end());
        old_value = has_old_value ? old_value_it->second : config_value;

        m_values[key] = config_value;

        if (meta_it != m_metas.end() && meta_it->second.change_callback) {
            has_change_callback = true;
            change_callback = meta_it->second.change_callback;
        }

        for (const auto& callback : m_global_change_callbacks) {
            pending_callbacks.push_back([callback, key, old_value, config_value]() {
                callback(key, old_value, config_value);
            });
        }
    }

    for (const auto& cb : pending_callbacks) {
        cb();
    }
    if (has_change_callback) {
        change_callback(config_value);
    }

    return ResultV2<void>::ok();
}

ResultV2<void> ConfigManager::load_from_file(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return ResultV2<void>::err(Error::Code::ResourceNotFound,
                                   std::format("Config file not found: {}", path.string()),
                                   path.string());
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return ResultV2<void>::err(Error::Code::ResourceNotFound,
                                   std::format("Failed to open config file: {}", path.string()),
                                   path.string());
    }

    // 空文件视为有效配置（无内容），优雅降级
    if (file.peek() == std::ifstream::traits_type::eof()) {
        file.close();
        return ResultV2<void>::ok();
    }

    try {
        nlohmann::json j;
        file >> j;
        file.close();

        // 递归展平嵌套 JSON（支持 {"backend": {"api_key": "..."}} 格式）
        // 同时向后兼容旧的扁平格式 {"backend.api_key": "..."}
        flatten_json(j, "", m_values);

        return ResultV2<void>::ok();

    } catch (const nlohmann::json::parse_error& e) {
        return ResultV2<void>::err(
            Error::Code::ConfigParseFailed,
            std::format("JSON parse error in {}: {}", path.string(), e.what()), path.string());
    } catch (const std::exception& e) {
        return ResultV2<void>::err(Error::Code::ConfigParseFailed,
                                   std::format("Error reading {}: {}", path.string(), e.what()),
                                   path.string());
    }
}

namespace {

/// 权限加固失败不阻断保存（可用性优先于加固），但必须留痕，便于用户排查为何权限未收紧。
void harden_file_or_warn(const std::filesystem::path& path) {
    auto result = harden_private_file(path);
    if (result.is_err()) {
        LOG_WARN("[config] 文件权限加固失败（配置已正常写入）path={} err={}", path.string(),
                 result.error().to_string());
    }
}

void harden_dir_or_warn(const std::filesystem::path& path) {
    auto result = harden_private_dir(path);
    if (result.is_err()) {
        LOG_WARN("[config] 目录权限加固失败 path={} err={}", path.string(),
                 result.error().to_string());
    }
}

}  // namespace

ResultV2<void> ConfigManager::save_to_file(const std::filesystem::path& path) {
    nlohmann::json j;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [key, value] : m_values) {
            // 将 dot 分隔的 key 写为嵌套 JSON（"backend.api_key" → j["backend"]["api_key"]）
            set_nested_json(j, key, value);
        }
    }

    try {
        // 确保父目录存在，并收紧为「仅属主可访问」——配置内容含 API Key 等敏感项
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
            harden_dir_or_warn(path.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return ResultV2<void>::err(
                Error::Code::ConfigParseFailed,
                std::format("Failed to create config file: {}", path.string()), path.string());
        }
        file << j.dump(4);
        file.close();

        // ofstream 直写（trunc）保留已存在文件的权限位，老用户的 0644 会一直留着，
        // 因此必须在 close() 之后显式收紧，而不能只在创建路径上设一次。
        harden_file_or_warn(path);
        return ResultV2<void>::ok();

    } catch (const std::exception& e) {
        return ResultV2<void>::err(Error::Code::ConfigParseFailed,
                                   std::format("Error writing {}: {}", path.string(), e.what()),
                                   path.string());
    }
}

void ConfigManager::add_change_callback(
    std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_change_callbacks.push_back(std::move(callback));
}

void ConfigManager::clear_change_callbacks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_change_callbacks.clear();
}

void ConfigManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_values.clear();
    m_metas.clear();
}

// L-5：clear_for_test() 已移除（补丁式 API，生产接口不应含测试专用方法）。
//      其实现与 clear() 完全一致，测试代码改为调用 clear()。

std::vector<std::string> ConfigManager::get_all_keys() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_values.size());
    for (const auto& [key, _] : m_values) {
        keys.push_back(key);
    }
    return keys;
}

// === C-2/C-4：结构化 Schema 实现（V2-1：返回 ResultV2）===

ResultV2<void> ConfigSchema::validate_value(const ConfigValue& value) const {
    // 类型校验
    switch (type) {
        case Type::Bool:
            if (!std::holds_alternative<bool>(value)) {
                return ResultV2<void>::err(
                    Error::Code::ConfigInvalid,
                    std::format("Expected bool, got {}", value.index() == 0   ? "bool"
                                                         : value.index() == 1 ? "int"
                                                         : value.index() == 2 ? "double"
                                                                              : "string"),
                    key);
            }
            break;
        case Type::Int:
            if (!std::holds_alternative<int>(value)) {
                return ResultV2<void>::err(Error::Code::ConfigInvalid, "Expected int", key);
            }
            if (int_range) {
                int v = std::get<int>(value);
                if (v < int_range->first || v > int_range->second) {
                    return ResultV2<void>::err(Error::Code::ConfigInvalid,
                                               std::format("Value {} out of range [{}, {}]", v,
                                                           int_range->first, int_range->second),
                                               key);
                }
            }
            break;
        case Type::Double:
            if (!std::holds_alternative<double>(value)) {
                return ResultV2<void>::err(Error::Code::ConfigInvalid, "Expected double", key);
            }
            if (double_range) {
                double v = std::get<double>(value);
                if (v < double_range->first || v > double_range->second) {
                    return ResultV2<void>::err(
                        Error::Code::ConfigInvalid,
                        std::format("Value {} out of range [{}, {}]", v, double_range->first,
                                    double_range->second),
                        key);
                }
            }
            break;
        case Type::String:
            if (!std::holds_alternative<std::string>(value)) {
                return ResultV2<void>::err(Error::Code::ConfigInvalid, "Expected string", key);
            }
            break;
        case Type::Enum:
            if (!std::holds_alternative<std::string>(value)) {
                return ResultV2<void>::err(Error::Code::ConfigInvalid, "Expected string for enum",
                                           key);
            }
            {
                const auto& s = std::get<std::string>(value);
                if (std::find(enum_values.begin(), enum_values.end(), s) == enum_values.end()) {
                    std::string allowed;
                    for (size_t i = 0; i < enum_values.size(); ++i) {
                        if (i > 0) allowed += ", ";
                        allowed += enum_values[i];
                    }
                    return ResultV2<void>::err(
                        Error::Code::ConfigInvalid,
                        std::format("Value '{}' not in enum [{}]", s, allowed), key);
                }
            }
            break;
    }

    // 自定义验证
    if (validate) {
        auto result = validate(value);
        if (result.is_err()) {
            return result;
        }
    }

    return ResultV2<void>::ok();
}

void ConfigManager::register_schema(ConfigSchema schema) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_schemas[schema.key] = std::move(schema);
}

ResultV2<ConfigSchema> ConfigManager::get_schema(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_schemas.find(key);
    if (it == m_schemas.end()) {
        return ResultV2<ConfigSchema>::err(Error::Code::ConfigMissing,
                                           std::format("Config schema '{}' not found", key), key);
    }
    return ResultV2<ConfigSchema>::ok(it->second);
}

std::vector<ConfigSchema> ConfigManager::get_all_schemas() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<ConfigSchema> result;
    result.reserve(m_schemas.size());
    for (const auto& [_, schema] : m_schemas) {
        result.push_back(schema);
    }
    return result;
}

void ConfigManager::load_from_env() {
    // C-4：遍历所有已注册 Schema，按 env_var 加载环境变量
    std::vector<std::pair<std::string, std::string>> env_bindings;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [key, schema] : m_schemas) {
            if (!schema.env_var.empty()) {
                env_bindings.emplace_back(key, schema.env_var);
            }
        }
    }

    for (const auto& [key, env_var] : env_bindings) {
        const char* val = std::getenv(env_var.c_str());
        if (val == nullptr) continue;

        // 查询 schema 获取类型
        auto schema_result = get_schema(key);
        if (schema_result.is_err()) continue;
        const auto& schema = schema_result.value();

        // 按类型解析
        ConfigValue parsed;
        try {
            switch (schema.type) {
                case ConfigSchema::Type::Bool:
                    parsed = (std::string(val) == "true" || std::string(val) == "1");
                    break;
                case ConfigSchema::Type::Int:
                    parsed = std::stoi(val);
                    break;
                case ConfigSchema::Type::Double:
                    parsed = std::stod(val);
                    break;
                case ConfigSchema::Type::String:
                case ConfigSchema::Type::Enum:
                    parsed = std::string(val);
                    break;
            }
        } catch (const std::exception&) {
            continue;  // 解析失败跳过
        }

        set_value(key, parsed);
    }
}

ConfigScope::ConfigScope(const std::string& prefix, IConfigReader& reader, IConfigWriter& writer)
    : m_prefix(prefix), m_reader(reader), m_writer(writer) {
    if (!m_prefix.empty() && m_prefix.back() != '.') {
        m_prefix += '.';
    }
}

ConfigScope::~ConfigScope() = default;

std::string ConfigScope::make_key(const std::string& key) const { return m_prefix + key; }

}  // namespace agent
