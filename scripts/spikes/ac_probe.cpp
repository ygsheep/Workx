// ac_probe.cpp — AppContainer 最小探针：写文件 + 退出码 42
// 用法: ac_probe.exe <out-file>
#include <windows.h>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 44;
    HANDLE f = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return 43;  // 文件创建失败（含 ACL 拒绝）
    DWORD n = 0;
    WriteFile(f, "ac-probe-ok\r\n", 12, &n, nullptr);
    CloseHandle(f);
    return 42;
}
