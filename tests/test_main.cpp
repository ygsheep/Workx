/**
 * @file test_main.cpp
 * @brief 测试入口（Catch2 Session；Windows 下自定义 wmain 以修正 argv 编码）
 *
 * 每个单元测试目标都会编译本文件（见 tests/unit/CMakeLists.txt 的 UNIT_TEST_COMMON）。
 */

#include <catch2/catch_session.hpp>
#include <catch2/internal/catch_leak_detector.hpp>

#if defined(_WIN32)
#include <windows.h>

#include <string>
#include <vector>
#endif

namespace {

/// @brief Catch2 的 MSVC 泄漏检测器实例
/// @details 上游把它定义在 Catch2WithMain 的 catch_main.cpp 里。本文件提供自己的
///          入口后，该目标文件不再被链接进来，故在此重新实例化以免丢失退出期泄漏报告。
///          LeakDetector 的构造/析构实现在核心库 catch_leak_detector.cpp，仍然可用。
const Catch::LeakDetector kLeakDetector;

}  // namespace

#if defined(_WIN32)

namespace {

/// @brief 宽字符（UTF-16）转 UTF-8
std::string to_utf8(const wchar_t* w) {
    const int need = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (need <= 1) return {};
    std::string out(static_cast<size_t>(need - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), need, nullptr, nullptr);
    return out;
}

}  // namespace

/// @brief Windows 入口：以 UTF-16 取命令行，转 UTF-8 后交给 Catch2
/// @details 为什么必须自己提供入口（而不是直接用 Catch2WithMain 的 main）：
///          Catch2 v3 的入口在 `catch_main.cpp` 里按 `defined(_UNICODE)` 二选一，
///          而 vcpkg 预编译的 Catch2WithMain **未定义 _UNICODE**，因此落地的是
///          `main(int, char** argv)` —— argv 由 CRT 从 ANSI 代码页（本机 936/GBK）
///          解码。但测试名是以 UTF-8（项目全局 `/utf-8`）编进二进制的，两者
///          **字节不通约**，于是任何含非 ASCII 字符的测试名都无法被按名选中：
///          ctest 会打印 "No test cases matched" 并把该用例判为 Failed。
///          （本仓已有 cmake/CatchAddTests_utf8.cmake 修发现阶段，但执行阶段
///            是把测试名当过滤参数传给本进程，必须在这一侧修。）
///          用 wmain 拿到未被转码的 UTF-16 命令行再转 UTF-8，两侧即一致。
extern "C" int __cdecl wmain(int argc, wchar_t* argv[]) {
    (void)&kLeakDetector;

    std::vector<std::string> storage;
    storage.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        storage.push_back(to_utf8(argv[i]));
    }
    std::vector<char*> utf8_argv;
    utf8_argv.reserve(storage.size());
    for (auto& s : storage) {
        utf8_argv.push_back(s.data());
    }

    return Catch::Session().run(static_cast<int>(utf8_argv.size()), utf8_argv.data());
}

#else

int main(int argc, char* argv[]) {
    (void)&kLeakDetector;
    return Catch::Session().run(argc, argv);
}

#endif
