/**
 * @file tool_registry.h
 * @brief ToolRegistry — 外部工具路径发现与缓存
 * @details 负责查找 rg 等外部 CLI 的实际路径，首次调用时探测，结果缓存。
 *          不管理进程生命周期（那是 subprocess::exec 的职责），不池化进程，
 *          只做一件事：rg 在哪？缓存答案。
 *
 *          查找顺序（对齐 Claude Code CLI）：
 *          1. 配置覆盖（config.tool_path.ripgrep）
 *          2. bundled（<exe_dir>/tools/rg(.exe)）
 *          3. PATH（which rg / where rg.exe）
 *          4. 全部失败：返回 nullopt，工具层回退 C++ 实现
 *
 *          单例模式：整个进程共享一份路径缓存，对齐 CC ripgrep.ts 的
 *          getRipgrepConfig = memoize(...) 模式。
 *
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace agent::process {

/// @brief 外部工具路径发现与缓存（单例）
/// @details 不管理进程，只缓存路径。线程安全。
class ToolRegistry {
   public:
    /// @brief 获取单例
    static ToolRegistry& instance();

    /// @brief 查找 ripgrep 路径
    /// @return rg 可执行文件绝对路径；未找到返回 nullopt
    /// @note 首次调用时探测，结果缓存（包括 nullopt：未找到也会被缓存，
    ///       后续调用直接返回 nullopt 而不重新探测，需 clear_cache() 重置）。
    ///       线程安全。
    std::optional<std::string> resolve_ripgrep() const;

    /// @brief 查找 jq 路径（bundled <exe_dir>/tools/jq > PATH）
    /// @return jq 可执行文件绝对路径；未找到返回 nullopt
    /// @note 结果缓存与 resolve_tool 一致，线程安全。
    std::optional<std::string> resolve_jq() const;

    /// @brief 查找指定工具路径（通用接口，未来扩展 fd/bat 等）
    /// @param tool_name 工具名（如 "ripgrep"）
    /// @param bundled_relative_path bundled 目录下相对路径（如 "tools/rg.exe"）
    /// @param path_name PATH 中查找的命令名（如 "rg" 或 "rg.exe"）
    /// @return 可执行文件绝对路径；未找到返回 nullopt
    /// @note 首次调用时探测，结果缓存（包括 nullopt：未找到也会被缓存）。
    std::optional<std::string> resolve_tool(const std::string& tool_name,
                                            const std::string& bundled_relative_path,
                                            const std::string& path_name) const;

    /// @brief 清除缓存（主要用于测试）
    void clear_cache();

    /// @brief 在 PATH 中查找可执行文件
    /// @param name 命令名（如 "nvim" 或 "nvim.exe"）
    /// @return 找到的绝对路径；未找到返回 nullopt
    /// @note Windows 上自动尝试 .exe 扩展名（PATH 中常省略扩展名）
    static std::optional<std::string> find_executable(const std::string& name);

    /// @brief 当前可执行文件所在目录
    /// @return 可执行文件目录的绝对路径（用于定位 bundled 资源，如 tools/、skills/bundled/）
    static std::string executable_dir();

   private:
    ToolRegistry() = default;

    /// @brief 获取当前可执行文件所在目录
    /// @return 可执行文件目录的绝对路径（用于查找 bundled tools/）
    static std::string get_executable_dir();

    /// @brief 在 PATH 中查找命令
    /// @param name 命令名（如 "rg" 或 "rg.exe"）
    /// @return 找到的绝对路径；未找到返回 nullopt
    static std::optional<std::string> find_in_path(const std::string& name);

    /// @brief 检查文件是否存在且可执行
    static bool is_executable(const std::string& path);

    mutable std::mutex m_mutex;
    /// @brief 缓存：tool_name → 路径（nullopt 表示已探测但未找到）
    mutable std::unordered_map<std::string, std::optional<std::string>> m_cache;
};

}  // namespace agent::process
