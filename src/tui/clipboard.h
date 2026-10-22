/**
 * @file clipboard.h
 * @brief 跨平台系统剪贴板写入（拖拽选中 → 自动复制）
 * @details Windows 用 Win32 CF_UNICODETEXT；POSIX 依次尝试 pbcopy / xclip / xsel。
 *          纯内联、无全局状态；写失败返回 false（POSIX 缺工具 / 非交互环境，
 *          调用方据此不弹"已复制"提示）。
 */

#pragma once

#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>

namespace ftxtui {

/// @brief 当前平台是否具备剪贴板写入能力（POSIX 探测一次后缓存）
inline bool clipboard_available();

/// @brief 把 UTF-8 文本写入系统剪贴板；失败返回 false
/// @details 成功时不释放分配的内存（系统接管）；失败时自行释放并复位剪贴板。
inline bool write_clipboard(const std::string& text);

#if defined(_WIN32)

inline bool clipboard_available() { return true; }

inline bool write_clipboard(const std::string& text) {
    if (text.empty()) return false;
    if (!OpenClipboard(nullptr)) return false;
    if (!EmptyClipboard()) {
        CloseClipboard();
        return false;
    }

    const int wlen =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (wlen <= 0) {
        CloseClipboard();
        return false;
    }
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (static_cast<size_t>(wlen) + 1) * sizeof(wchar_t));
    if (!mem) {
        CloseClipboard();
        return false;
    }
    wchar_t* dst = static_cast<wchar_t*>(GlobalLock(mem));
    if (dst) {
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), dst, wlen);
        dst[wlen] = L'\0';
        GlobalUnlock(mem);
    }
    if (!SetClipboardData(CF_UNICODETEXT, mem)) {
        GlobalFree(mem);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

#else  // POSIX

namespace clipboard_detail {

/// @brief 探测可用的剪贴板命令（0=无；1=pbcopy；2=xclip；3=xsel）。缓存结果。
inline int probe_tool() {
    static int tool = -1;
    if (tool >= 0) return tool;
    tool = 0;
    auto has = [](const char* name) {
        std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
        return std::system(cmd.c_str()) == 0;
    };
    if (has("pbcopy"))
        tool = 1;
    else if (has("xclip"))
        tool = 2;
    else if (has("xsel"))
        tool = 3;
    return tool;
}

}  // namespace clipboard_detail

inline bool clipboard_available() { return clipboard_detail::probe_tool() != 0; }

inline bool write_clipboard(const std::string& text) {
    if (text.empty()) return false;
    FILE* pipe = nullptr;
    switch (clipboard_detail::probe_tool()) {
        case 1:
            pipe = popen("pbcopy", "w");
            break;
        case 2:
            pipe = popen("xclip -selection clipboard", "w");
            break;
        case 3:
            pipe = popen("xsel --clipboard --input", "w");
            break;
        default:
            return false;
    }
    if (!pipe) return false;
    const size_t written = std::fwrite(text.data(), 1, text.size(), pipe);
    const int rc = pclose(pipe);
    return written == text.size() && rc == 0;
}

#endif  // _WIN32

}  // namespace ftxtui