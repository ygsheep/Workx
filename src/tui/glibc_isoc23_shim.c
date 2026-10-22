// glibc strtol 家族兼容 shim
// @date 2026-10
//
// 背景：ubuntu:24.04（GCC 13 / glibc 2.39）编译出的 workx 引用了
// __isoc23_strtol / __isoc23_strtoul / __isoc23_strtoll / __isoc23_strtoull
// 四个 GLIBC_2.38 新符号，拷到老 glibc 系统（debian:bookworm 2.36，
// terminal-bench 数据集大量使用）上载入即失败：
//   ./workx: /lib/x86_64-linux-gnu/libc.so.6: version `GLIBC_2.38' not found
//
// 本文件提供这四个符号的强定义，转发到经典 strtol 家族。链接器解析本程序
// 自身的未定义引用时优先命中可执行文件内的定义；glibc 2.38+ 系统上 libc 中
// 的同名版本化符号在共享库内，与可执行文件中的定义不冲突。
//
// ⚠️ 实现约定（改前想清楚，2026-10-12 跑分实测验证过）：
//   1. 必须以 C 语言编译（.c 文件 CMake 天然走 C 编译器）。用 g++ 编会预定义
//      _GNU_SOURCE，激活 glibc 头文件的重定向路径，转发可能变成自递归。
//   2. 刻意不 #include <stdlib.h>：glibc 2.38+ 头文件会把 strtol 声明
//      __REDIRECT 到 __isoc23_strtol，include 之后本文件内的转发调用会被
//      重定向到自身。手写原型彻底绕开这组宏。
//   3. 配套链接选项 -static-libstdc++ -static-libgcc
//      （见 src/tui/CMakeLists.txt）。两者缺一不可：只加 shim 不静态化
//      libstdc++ 时，GLIBCXX 动态版本要求仍然超标，bookworm 依旧载入失败。

// 手写原型（勿改为 #include <stdlib.h>，见上方注释 2）
extern long strtol(const char* nptr, char** endptr, int base);
extern unsigned long strtoul(const char* nptr, char** endptr, int base);
extern long long strtoll(const char* nptr, char** endptr, int base);
extern unsigned long long strtoull(const char* nptr, char** endptr, int base);

long __isoc23_strtol(const char* nptr, char** endptr, int base) {
    return strtol(nptr, endptr, base);
}

unsigned long __isoc23_strtoul(const char* nptr, char** endptr, int base) {
    return strtoul(nptr, endptr, base);
}

long long __isoc23_strtoll(const char* nptr, char** endptr, int base) {
    return strtoll(nptr, endptr, base);
}

unsigned long long __isoc23_strtoull(const char* nptr, char** endptr, int base) {
    return strtoull(nptr, endptr, base);
}
