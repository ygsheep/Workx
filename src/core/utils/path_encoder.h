/**
 * @file path_encoder.h
 * @brief 项目路径编码工具
 * @details 将项目路径编码为目录名，对齐 cc 的 projects 目录命名方案。
 *          路径分隔符(\/)和盘符冒号(:)全替换为'-'。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <filesystem>

namespace core::util {

/// @brief 将项目路径编码为目录名（对齐 cc 的 projects 目录命名）
/// @param path 项目绝对路径
/// @return 编码后的字符串（路径分隔符 \ / 和盘符冒号 : 全替换为 -）
/// @details 例：D:\develop\workx → D--develop-workx
///          /home/user/workx → -home-user-workx
std::string encode_project_path(const std::filesystem::path& path);

}  // namespace core::util
