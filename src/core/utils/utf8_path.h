/**
 * @file utf8_path.h
 * @brief 文件系统路径 → UTF-8 字符串（跨平台）
 * @details Windows 上 `std::filesystem::path::string()` 返回的是**本机窄编码**
 *          （中文系统即 GBK/ACP），而不是 UTF-8。把这种字节写进 UTF-8 语境
 *          （提示词、JSON 请求体、注册文件）会产生非法 UTF-8 序列，进而让
 *          `nlohmann::json::dump()` 抛 `type_error.316` —— 表现为请求发不出去
 *          （会话零输出）甚至启动即崩（#182）。
 *          需要把路径交给模型、JSON 或跨进程协议时，一律用本函数取字符串。
 *          POSIX 上路径本就是 UTF-8 字节，直接返回 `string()`。
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <filesystem>
#include <string>

namespace core::util {

/// @brief 把路径转成 UTF-8 字符串
/// @param path 任意路径（可为空）
/// @return UTF-8 编码的字符串；Windows 转换失败时退回本机窄编码（不抛异常）
std::string path_to_utf8(const std::filesystem::path& path);

}  // namespace core::util
