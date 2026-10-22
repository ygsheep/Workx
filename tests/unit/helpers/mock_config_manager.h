/**
 * @file mock_config_manager.h
 * @brief 测试用 Mock IConfigManager
 * @details 基于内存 map 的配置存储，支持预设值、读取、写入、查询操作。
 *          供需要注入 IConfigManager 的组件（Terminal/ChatSession/工具等）做隔离测试。
 *
 * 使用示例：
 * @code
 *   using namespace agent::test;
 *   MockConfigManager cfg;
 *   cfg.set_value("backend.model_name", std::string("test-model"));
 *   REQUIRE(cfg.get_or<std::string>("backend.model_name", "") == "test-model");
 * @endcode
 */

#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>

#include "core/config/i_config_manager.h"

namespace agent::test {

/// @brief Mock IConfigManager
/// @details 线程安全（内部互斥锁）。所有数据存内存，load_from_file/save_to_file
///          默认返回成功但不实际 I/O，可通过 set_load_error 注入错误场景。
class MockConfigManager final : public IConfigManager {
   public:
    MockConfigManager() = default;
    ~MockConfigManager() override = default;

    MockConfigManager(const MockConfigManager&) = delete;
    MockConfigManager& operator=(const MockConfigManager&) = delete;

    // === IConfigManager 实现（V2-1：返回 ResultV2）===

    [[nodiscard]] bool has(const std::string& key) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_values.count(key) > 0;
    }

    [[nodiscard]] ResultV2<ConfigValue> get_value(const std::string& key) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_values.find(key);
        if (it == m_values.end()) {
            return ResultV2<ConfigValue>::err(Error::Code::ConfigMissing, "Key not found: " + key,
                                              key);
        }
        return ResultV2<ConfigValue>::ok(it->second);
    }

    ResultV2<void> set_value(const std::string& key, ConfigValue value) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_values[key] = std::move(value);
        return ResultV2<void>::ok();
    }

    ResultV2<void> remove_value(const std::string& key) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_values.erase(key);
        return ResultV2<void>::ok();
    }

    ResultV2<void> load_from_file(const std::filesystem::path& /*path*/) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_load_error) {
            return ResultV2<void>::err(Error::Code::ConfigParseFailed, *m_load_error);
        }
        return ResultV2<void>::ok();
    }

    ResultV2<void> save_to_file(const std::filesystem::path& /*path*/) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_save_error) {
            return ResultV2<void>::err(Error::Code::ConfigParseFailed, *m_save_error);
        }
        return ResultV2<void>::ok();
    }

    [[nodiscard]] std::vector<std::string> get_all_keys() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> keys;
        keys.reserve(m_values.size());
        for (const auto& [k, _] : m_values) {
            keys.push_back(k);
        }
        return keys;
    }

    // === 测试辅助 API ===

    /// @brief 预设配置值（类型安全，便于测试构造）
    template <typename T>
    void set(const std::string& key, T value) {
        set_value(key, ConfigValue(std::move(value)));
    }

    /// @brief 注入 load_from_file 的错误（nullopt 清除错误）
    void set_load_error(std::optional<std::string> err) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_load_error = std::move(err);
    }

    /// @brief 注入 save_to_file 的错误（nullopt 清除错误）
    void set_save_error(std::optional<std::string> err) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_save_error = std::move(err);
    }

    /// @brief 清空所有配置
    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_values.clear();
        m_load_error.reset();
        m_save_error.reset();
    }

    /// @brief 获取已存储的键值对数量
    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_values.size();
    }

   private:
    mutable std::mutex m_mutex;
    std::map<std::string, ConfigValue> m_values;
    std::optional<std::string> m_load_error;
    std::optional<std::string> m_save_error;
};

}  // namespace agent::test
