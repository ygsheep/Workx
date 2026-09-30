/**
 * @file file_permissions.h
 * @brief 私有文件/目录权限加固（跨平台）
 * @details 把存放敏感内容的配置文件（API Key 等）与所在目录收紧为「仅属主可访问」。
 *          POSIX 走 chmod（0600 / 0700）；Windows 重建只含「属主 + SYSTEM」的 DACL，
 *          并置 PROTECTED 标志——否则父目录的继承 ACE 会被重新注入，加固立刻失效。
 * @version 1.0.0
 * @date 2026-10
 */

#pragma once

#include <filesystem>

#include "core/utils/result_v2.h"

namespace agent {

/// @brief 将文件收紧为「仅属主可读写」（POSIX 0600；Windows 属主 + SYSTEM 全控）
/// @param path 目标文件，必须已存在（本函数不负责创建）
/// @return 成功 ok；目标不存在返回 ResourceNotFound，加固失败返回 PermissionDenied
/// @note 已存在的文件同样会被收紧。`ofstream` 直写不会改变原权限位，故写后必须显式调用。
ResultV2<void> harden_private_file(const std::filesystem::path& path);

/// @brief 将目录收紧为「仅属主可访问」（POSIX 0700；Windows 属主 + SYSTEM 全控）
/// @param path 目标目录，必须已存在（本函数不负责创建），且不递归处理子项
/// @return 成功 ok；目标不存在返回 ResourceNotFound，加固失败返回 PermissionDenied
ResultV2<void> harden_private_dir(const std::filesystem::path& path);

}  // namespace agent
