/**
 * @file path_encoder.cpp
 * @brief 项目路径编码工具实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/utils/path_encoder.h"

#include <algorithm>

namespace core::util {

std::string encode_project_path(const std::filesystem::path& path) {
    std::string s = path.string();
    if (s.empty()) return s;
    // 路径分隔符和盘符冒号全替换为 '-'（对齐 cc 的 projects 目录命名）
    std::replace(s.begin(), s.end(), '\\', '-');
    std::replace(s.begin(), s.end(), '/', '-');
    std::replace(s.begin(), s.end(), ':', '-');
    return s;
}

}  // namespace core::util
