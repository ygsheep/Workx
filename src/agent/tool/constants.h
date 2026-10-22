/**
 * @file constants.h
 * @brief 工具系统常量定义
 * @details 工具模块共享的编译期常量：文件大小限制、行数限制、PDF 分页上限等
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <cstddef>

namespace agent::tool::constants {

/// 文件读取最大字节数（2MB），超过此大小拒绝读取，需使用 offset/limit 分段
constexpr size_t MAX_FILE_SIZE_BYTES = 2 * 1024 * 1024;

/// 默认最大读取行数，未指定 limit 时生效
constexpr int MAX_LINES_TO_READ = 2000;

}  // namespace agent::tool::constants
