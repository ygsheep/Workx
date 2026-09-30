/**
 * @file input_history.h
 * @brief 输入历史持久化（上下箭头浏览 + JSON 落盘）
 * @details 条目顺序 旧 → 新；导航位置 m_pos == size() 表示"草稿位"（未选中
 *          任何条目，即用户正在编辑的新输入）。首次离开草稿位时记录草稿，
 *          下箭头回到草稿位时还原。
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ftxtui {

/// @brief 输入历史：内存导航 + JSON 文件持久化
class InputHistory {
   public:
    /// @brief 从磁盘加载（文件缺失/损坏时静默回退为空）
    void load(const std::filesystem::path& path);
    /// @brief 保存到磁盘（创建父目录；失败静默）
    void save() const;

    /// @brief 追加一条历史（与最近一条相同则跳过；超过上限丢弃最旧）
    void push(const std::string& text);

    /// @brief 上一条（更旧）；首次离开草稿位时以 current 记录草稿
    /// @return false = 无历史或已到最旧
    bool prev(const std::string& current, std::string& out);
    /// @brief 下一条（更新）；到达草稿位返回草稿；已在草稿位返回 false
    bool next(std::string& out);
    /// @brief 复位导航位置到草稿位
    void reset_nav();

    size_t size() const { return m_entries.size(); }

   private:
    std::vector<std::string> m_entries;  ///< 旧 → 新
    size_t m_pos = 0;                    ///< 导航位置（== size() 为草稿位）
    std::string m_draft;                 ///< 离开草稿位前的输入
    std::filesystem::path m_path;
};

}  // namespace ftxtui
