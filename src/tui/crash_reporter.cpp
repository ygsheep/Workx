#include "crash_reporter.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <crtdbg.h>
#pragma comment(lib, "Dbghelp.lib")
#endif

#include <typeinfo>
#include <exception>

namespace crash {

namespace {

std::FILE* g_out = nullptr;

/// @brief 打开崩溃日志（~/.workx/logs/workx_crash.log），避免依赖 agent 日志组件。
std::FILE* OpenLog() {
#if defined(_WIN32)
    const char* home = std::getenv("USERPROFILE");
#else
    const char* home = std::getenv("HOME");
#endif
    std::string dir = home ? std::string(home) : std::string("");
    if (dir.empty()) return nullptr;
    dir += "/.workx/logs";
#if defined(_WIN32)
    CreateDirectoryA(dir.c_str(), nullptr);  // 已存在则失败无害
#endif
    std::string path = dir + "/workx_crash.log";
    return std::fopen(path.c_str(), "a");
}

void InitSymbols() {
#if defined(_WIN32)
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
#endif
}

std::atomic<bool> g_dumping{false};

/// @brief 将当前帧堆栈符号化并写入日志文件。
void WriteBacktrace(std::FILE* out) {
#if defined(_WIN32)
    if (!out) return;
    void* frames[40];
    const USHORT n = CaptureStackBackTrace(
        1, static_cast<DWORD>(sizeof(frames) / sizeof(frames[0])), frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        char sym_name[sizeof(SYMBOL_INFO) + 255];
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(sym_name);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 displ = 0;
        std::fprintf(out, "  #%02u 0x%08I64X", i, addr);
        if (SymFromAddr(GetCurrentProcess(), addr, &displ, sym)) {
            std::fprintf(out, "  %s+0x%llX", sym->Name, static_cast<unsigned long long>(displ));
            DWORD line = 0;
            IMAGEHLP_LINE64 li{};
            li.SizeOfStruct = sizeof(li);
            if (SymGetLineFromAddr64(GetCurrentProcess(), addr, &line, &li)) {
                std::fprintf(out, "  [%s:%u]", li.FileName, li.LineNumber);
            }
        }
        std::fprintf(out, "\n");
    }
    std::fflush(out);
#else
    (void)out;
#endif
}

/// @brief 统一落盘：标题 + 可选信息 + 堆栈。
void Dump(const char* title, const char* extra) {
    bool expected = false;
    if (!g_dumping.compare_exchange_strong(expected, true)) return;  // 防止重入
    std::FILE* out = g_out ? g_out : OpenLog();
    if (out) {
        std::fprintf(out, "\n===== crash: %s", title);
        if (extra && *extra) std::fprintf(out, " — %s", extra);
        std::fprintf(out, " =====\n");
        WriteBacktrace(out);
        std::fprintf(out, "====================\n");
        std::fflush(out);
    }
    g_dumping.store(false);
}

#if defined(_WIN32)
LONG WINAPI SehHandler(EXCEPTION_POINTERS* info) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "SEH 0x%08lX", info->ExceptionRecord->ExceptionCode);
    Dump("seh", buf);
    return EXCEPTION_EXECUTE_HANDLER;
}

LONG __stdcall VectoredHandler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // 仅记录真正需要关注的异常（访问违例/非法指令/整数溢出等），
    // 忽略断点等正常调试事件，避免误报。
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_STACK_OVERFLOW) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "first-chance SEH 0x%08lX @0x%p", code,
                      info->ExceptionRecord->ExceptionAddress);
        Dump("vectored", buf);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

void SigAbortHandler(int /*sig*/) { Dump("abort/sigabrt", "assert() failed or std::abort()"); }

void TerminateHandler() noexcept {
    std::string what = "unknown";
#if defined(_WIN32)
    try {
        throw;  // rethrow 当前异常以识别其类型
    } catch (const std::exception& e) {
        what = std::string(typeid(e).name()) + ": " + e.what();
    } catch (...) {
        what = "non-std exception";
    }
#else
    try {
        throw;
    } catch (const std::exception& e) {
        what = std::string(typeid(e).name()) + ": " + e.what();
    } catch (...) {
        what = "non-std exception";
    }
#endif
    Dump("terminate", what.c_str());
    std::abort();
}

#if defined(_WIN32)
void InvalidParamHandler(const wchar_t* expr, const wchar_t* func, const wchar_t* file,
                         unsigned line, uintptr_t) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "invalid_parameter expr=%ls func=%ls file=%ls line=%u",
                  expr ? expr : L"", func ? func : L"", file ? file : L"", line);
    Dump("invalid_parameter", buf);
}
#endif

}  // namespace

void InstallHandlers() {
#if defined(_WIN32)
    g_out = OpenLog();
    // 心跳：确认处理器已装配并可写日志（便于排查 handler 未触发的问题）
    if (g_out) {
        std::fprintf(g_out, "\n===== crash reporter installed =====\n");
        std::fflush(g_out);
    }
    InitSymbols();
    // assert 打到 stderr（桌面自动化可捕获），避免调试断言弹框挂起
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _set_invalid_parameter_handler(&InvalidParamHandler);
    SetUnhandledExceptionFilter(&SehHandler);
    AddVectoredExceptionHandler(1, &VectoredHandler);
#endif
    std::set_terminate(&TerminateHandler);
    std::signal(SIGABRT, &SigAbortHandler);
}

void DumpNow(const char* reason) { Dump("manual", reason ? reason : ""); }

void ReinstallSignalHandlers() {
    // FTXUI Internal::Install() 里为 SIGABRT/SIGSEGV 等注册了恢复终端用的处理器，
    // 会覆盖上面 InstallHandlers 里的信号处理；此处在其后重新接管 SIGABRT，
    // 保证 assert()/abort() 仍能落盘调用栈而非静默终止。
    std::signal(SIGABRT, &SigAbortHandler);
}

}  // namespace crash