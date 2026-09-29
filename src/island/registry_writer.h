/**
 * @file registry_writer.h
 * @brief TUI 注册文件读写（~/.workx/island.registry）
 * @details 每个 TUI 启动时写入一条记录：{pid, endpoint, project_root,
 *          started_at, model, last_heartbeat}。GUI 启动时扫描该文件列出
 *          所有活跃 TUI（pid 存活 + heartbeat 新鲜度）。写文件用
 *          临时文件 + rename 原子替换，避免并发读看到半成品。
 *          文件损坏时读取方跳过该文件（仅日志），不阻塞启动。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/utils/result_v2.h"

namespace island {

/// @brief 注册文件中的单条会话记录
struct RegistryEntry {
    uint32_t pid = 0;            ///< TUI 进程 id（存活检测）
    std::string endpoint;        ///< IPC 端点路径（\\.\pipe\... / unix socket）
    std::string project_root;    ///< TUI 工作目录
    int64_t started_at = 0;      ///< 启动时间（Unix 秒）
    std::string model;           ///< 当前模型名
    int64_t last_heartbeat = 0;  ///< 最近心跳时间（Unix 秒，GUI 每 5s ping 刷新）
};

/// @brief 注册文件读写器
class RegistryWriter {
   public:
    /// @param path 注册文件路径（默认 default_registry_path()）
    explicit RegistryWriter(std::filesystem::path path);

    /// @brief 写入/更新当前进程记录（原子替换整个文件，保留其他 pid 记录）
    /// @return 写文件失败返回 Error（IOError 类）
    [[nodiscard]] agent::ResultV2<void> write(const RegistryEntry& entry);

    /// @brief 移除指定 pid 的记录（TUI 退出时清理）
    [[nodiscard]] agent::ResultV2<void> remove(uint32_t pid);

    /// @brief 读取全部记录；文件不存在返回空列表（非错误）
    [[nodiscard]] static agent::ResultV2<std::vector<RegistryEntry>> read_all(
        const std::filesystem::path& path);

    /// @brief 平台相关默认路径：%USERPROFILE%/.workx/island.registry
    ///        POSIX: $HOME/.workx/island.registry
    [[nodiscard]] static std::filesystem::path default_registry_path();

    /// @brief 序列化为 JSON（{sessions:[...]}，供 GUI）
    [[nodiscard]] static nlohmann::json to_json(const std::vector<RegistryEntry>& entries);

    /// @brief 解析文件内容；损坏/格式错误返回 Error（ConfigParseFailed）
    [[nodiscard]] static agent::ResultV2<std::vector<RegistryEntry>> parse(const std::string& text);

   private:
    std::filesystem::path m_path;
};

}  // namespace island