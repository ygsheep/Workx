/**
 * @file wizard.h
 * @brief 首次运行设置向导（#66）
 * @details 配置文件不存在时由 main 在启动会话前调用：选择服务提供商 →
 *          填写 API Key → 确认上下文长度，完成后写入 config.json。
 */

#pragma once

#include <filesystem>

namespace agent {
class ConfigManager;
}  // namespace agent

namespace ftxtui {

/// @brief 向导配置结果（apply_wizard_config 的输入）
struct WizardConfig {
    std::string provider;     ///< 预设内部名（如 "deepseek"）
    std::string api_key;      ///< API Key（可空）
    std::string custom_url;   ///< 自定义 URL 预设的完整端点（可空）
    std::string context_len;  ///< 上下文长度字符串（可空；非数字忽略）
};

/// @brief 将向导结果写入配置管理器并落盘
/// @details 写入 backend.provider / api_key / model_name（预设默认模型）/
///          context_length / remote_url（自定义预设），再 save_to_file。
/// @return true = 配置写入成功
bool apply_wizard_config(agent::ConfigManager& cfg, const std::filesystem::path& config_path,
                         const WizardConfig& wc);

/// @brief 运行首次运行设置向导（阻塞，独立 FTXUI 全屏界面）
/// @param cfg 配置管理器（完成后经 apply_wizard_config 写入）
/// @param config_path 配置文件路径
/// @return true = 用户完成配置并写入；false = 用户跳过/取消
bool run_first_run_wizard(agent::ConfigManager& cfg, const std::filesystem::path& config_path);

}  // namespace ftxtui
