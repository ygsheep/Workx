/**
 * @file icons.h
 * @brief 图标层：Nerd Font 私有区字符 ↔ ASCII 降级（A5 可访问性）
 * @details Nerd Font 图标（U+F000 私有区）在无对应字体的终端显示为豆腐块。
 *          提供全局开关（默认开启），关闭后所有私有区字符降级为 ASCII/空。
 *          Braille 旋转符、✓/✖ 等通用 Unicode 不在此列（多数终端支持）。
 */

#pragma once

#include <string_view>

namespace ftxtui::theme {

/// @brief 内部标志（static 局部，避免头文件 ODR 问题）
inline bool& nerd_font_flag() {
    static bool flag = true;
    return flag;
}

/// @brief 设置 Nerd Font 图标开关（启动时由 main 解析 --ascii 设置）
inline void set_nerd_font(bool enabled) { nerd_font_flag() = enabled; }

/// @brief Nerd Font 图标是否开启
inline bool nerd_font() { return nerd_font_flag(); }

/// @brief 取图标：开启时用 Nerd Font 字形，否则用 ASCII 降级
inline std::string_view icon(std::string_view nf, std::string_view ascii) {
    return nerd_font() ? nf : ascii;
}

// ----------------------------------------------------------------------------
// 常用图标（Nerd Font 私有区 → ASCII 降级）
// ----------------------------------------------------------------------------

/// @brief 思考（灯泡 nf-fa-lightbulb）→ 无图标
inline std::string_view icon_think() { return icon("\uF0EB", ""); }
/// @brief 展开指示（chevron-down）→ "v"
inline std::string_view icon_chevron_down() { return icon("\uF078", "v"); }
/// @brief 收起指示（chevron-right）→ ">"
inline std::string_view icon_chevron_right() { return icon("\uF054", ">"); }
/// @brief 工具（扳手 nf-fa-wrench）→ 无图标
inline std::string_view icon_tool() { return icon("\uF0AD", ""); }
/// @brief 技能（魔杖 nf-fa-magic-wand-sparkles）→ "S"
inline std::string_view icon_skill() { return icon("\uF0D0", "S"); }
/// @brief 计划模式（剪贴板 nf-fa-clipboard）→ "P"
inline std::string_view icon_plan() { return icon("\uF044", "P"); }
/// @brief 完全访问（key nf-cod-key）→ "A"
inline std::string_view icon_full_access() { return icon("\uEB53", "A"); }
/// @brief 手动审批（hand nf-fa-hand-paper）→ "?"
inline std::string_view icon_manual() { return icon("\uF256", "?"); }
/// @brief 复制（nf-fa-copy）→ "C"
inline std::string_view icon_copy() { return icon("\uF0C5", "C"); }
/// @brief 重试（nf-fa-rotate-right）→ "R"
inline std::string_view icon_retry() { return icon("\uF2F9", "R"); }
/// @brief 状态点（nf-cod-circle_small_filled，小实心点）→ "·"（MCP 连接状态：绿/红/灰）
inline std::string_view icon_dot() { return icon("\uEB8A", "·"); }

// ----------------------------------------------------------------------------
// 项目文件树（项目 tab：JetBrains 风格文件图标，Nerd Font FA 文件类字形）
// ----------------------------------------------------------------------------

/// @brief 文件夹（nf-fa-folder）→ 无（ASCII 下隐藏，保留缩进与 chevron 结构）
inline std::string_view icon_folder() { return icon("\uF07B", ""); }
/// @brief 展开的文件夹（nf-fa-folder-open）→ 无
inline std::string_view icon_folder_open() { return icon("\uF07C", ""); }
/// @brief 通用文件（nf-fa-file-o）→ 无
inline std::string_view icon_file_default() { return icon("\uF016", ""); }
/// @brief 文本文件（nf-fa-file-text-o）→ 无
inline std::string_view icon_file_text() { return icon("\uF15C", ""); }
/// @brief 代码文件（nf-fa-file-code-o）→ 无
inline std::string_view icon_file_code() { return icon("\uF1C9", ""); }
/// @brief 配置文件（nf-fa-cog，齿轮）→ 无
inline std::string_view icon_file_config() { return icon("\uF013", ""); }
/// @brief 图片（nf-fa-file-image-o）→ 无
inline std::string_view icon_file_image() { return icon("\uF1C5", ""); }
/// @brief 归档（nf-fa-file-archive-o）→ 无
inline std::string_view icon_file_archive() { return icon("\uF1C6", ""); }
/// @brief 音频（nf-fa-file-audio-o）→ 无
inline std::string_view icon_file_audio() { return icon("\uF1C7", ""); }
/// @brief 视频（nf-fa-file-video-o）→ 无
inline std::string_view icon_file_video() { return icon("\uF1C8", ""); }
/// @brief 数据库（nf-fa-database）→ 无
inline std::string_view icon_file_database() { return icon("\uF1C0", ""); }
/// @brief 可执行/脚本（nf-fa-terminal）→ 无
inline std::string_view icon_file_terminal() { return icon("\uF120", ""); }
/// @brief HTML（nf-fa-code）→ 无
inline std::string_view icon_file_html() { return icon("\uF121", ""); }

/// @brief 按扩展名（小写、无点）取文件类型图标；未知返回通用文件图标
inline std::string_view icon_file_by_ext(const std::string& ext) {
    if (ext == "c" || ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "h" || ext == "hpp" ||
        ext == "hh" || ext == "cs" || ext == "java" || ext == "py" || ext == "js" || ext == "jsx" ||
        ext == "ts" || ext == "tsx" || ext == "go" || ext == "rs" || ext == "swift" ||
        ext == "kt" || ext == "m" || ext == "mm" || ext == "scala" || ext == "php" ||
        ext == "lua" || ext == "rb" || ext == "vue" || ext == "sh" || ext == "bash" ||
        ext == "zsh" || ext == "ps1" || ext == "bat" || ext == "cmd" || ext == "css" ||
        ext == "scss" || ext == "less" || ext == "sass")
        return icon_file_code();
    if (ext == "html" || ext == "htm") return icon_file_html();
    if (ext == "md" || ext == "markdown" || ext == "rst") return icon_file_text();
    if (ext == "txt" || ext == "log") return icon_file_text();
    if (ext == "json" || ext == "yaml" || ext == "yml" || ext == "toml" || ext == "ini" ||
        ext == "conf" || ext == "cfg" || ext == "config" || ext == "xml" || ext == "cmake")
        return icon_file_config();
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "svg" ||
        ext == "webp" || ext == "ico" || ext == "bmp")
        return icon_file_image();
    if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "tgz" || ext == "rar" || ext == "7z")
        return icon_file_archive();
    if (ext == "mp3" || ext == "wav" || ext == "ogg" || ext == "flac" || ext == "m4a")
        return icon_file_audio();
    if (ext == "mp4" || ext == "mkv" || ext == "avi" || ext == "mov" || ext == "webm")
        return icon_file_video();
    if (ext == "db" || ext == "sql" || ext == "sqlite" || ext == "sqlite3")
        return icon_file_database();
    return icon_file_default();
}

// ----------------------------------------------------------------------------
// 模型输入能力（供应商管理面板：Nerd Font → ASCII 降级）
// ----------------------------------------------------------------------------

/// @brief 文本输入（nf-fa-font）→ "T"（LLM 恒支持）
inline std::string_view icon_input_text() { return icon("\uF031", "T"); }
/// @brief 图像/视觉输入（nf-fa-image）→ "图"（supports_vision）
inline std::string_view icon_input_vision() { return icon("\uF03E", "图"); }

}  // namespace ftxtui::theme