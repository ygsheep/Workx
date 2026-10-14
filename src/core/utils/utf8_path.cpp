/**
 * @file utf8_path.cpp
 * @brief 文件系统路径 → UTF-8 字符串实现（见 utf8_path.h）
 */

#include "core/utils/utf8_path.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace core::util {

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    // Windows：path::native() 是 UTF-16 宽字符串，显式转 UTF-8。
    // 不用 path::string()：那是本机窄编码（GBK/ACP），中文路径会产生非法 UTF-8。
    const std::wstring& wide = path.native();
    if (wide.empty()) return {};

    const int src_len = static_cast<int>(wide.size());
    const int len =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), src_len, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return path.string();  // 转换失败：退回本机窄编码兜底

    std::string out(static_cast<size_t>(len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), src_len, out.data(), len, nullptr, nullptr);
    return out;
#else
    // POSIX：窄字符串即 UTF-8 字节
    return path.string();
#endif
}

}  // namespace core::util
