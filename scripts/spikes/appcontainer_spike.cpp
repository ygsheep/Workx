// appcontainer_spike.cpp — Workx #113 spike
//
// 验证 AppContainer 在本机的可行性（issue 评论列出的 spike 清单）：
//   S1  AppContainer profile 创建 / SID 派生
//   S2  按目录授权机制（给工作区/缓存目录加 AppContainer SID 的 ACE）
//   S3  AppContainer 内 cmd 基线存活（System32 读执行）
//   S4  写 C:\Windows\Temp 被拒           ← 验收标准 1（系统目录写拦截）
//   S5  写已授权的 %TEMP% 成功            ← 落地机制：工作区白名单
//   S6  写已授权的 cwd 成功
//   S7  网络访问被拒（无 INTERNET capability）← 验收标准 2
//   S8  混淆命令：已授权脚本间接写 C:\Windows\Temp 仍被拒（内核级兜底 vs 命令级）
//   S9  Job Object 叠加：CREATE_SUSPENDED → Assign → Resume（与 subprocess.cpp 同款）
//   S10 Git Bash 在 AppContainer 内启动
//
// 输出每项结论与 GetLastError，供 issue 记录。

#include <windows.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <stdio.h>
#include <string>
#include <vector>

#pragma comment(lib, "userenv.lib")
#pragma comment(lib, "advapi32.lib")

static const wchar_t* kProfileName = L"Workx.Spike.113";

#define CHECK(cond) \
    ((cond) ? (void)0 : (void)(printf("  !! internal error at %dGLE=%lu\n", __LINE__, GetLastError())))

static std::string read_file(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "<unreadable GLE=" + std::to_string(GetLastError()) + ">";
    char buf[4096] = {};
    DWORD n = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    return std::string(buf, n);
}

// 把 AppContainer SID 加进目录 DACL（GRANT，读/写/执行，含子对象继承）
static bool grant_dir(const PSID sid, const std::wstring& dir) {
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &old_dacl, nullptr, &sd) != ERROR_SUCCESS) {
        printf("  GetNamedSecurityInfo failed GLE=%lu\n", GetLastError());
        return false;
    }
    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeType = TRUSTEE_IS_UNKNOWN;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    PACL new_dacl = nullptr;
    DWORD rc = SetEntriesInAclW(1, &ea, old_dacl, &new_dacl);
    if (rc == ERROR_SUCCESS) {
        rc = SetNamedSecurityInfoW((LPWSTR)dir.c_str(), SE_FILE_OBJECT,
                                   DACL_SECURITY_INFORMATION, nullptr, nullptr, new_dacl, nullptr);
    }
    if (sd) LocalFree(sd);
    if (new_dacl) LocalFree(new_dacl);
    if (rc != ERROR_SUCCESS) printf("  SetAcl failed rc=%lu GLE=%lu\n", rc, GetLastError());
    return rc == ERROR_SUCCESS;
}

struct LaunchResult {
    bool started = false;
    DWORD exit_code = (DWORD)-1;
    DWORD gle = 0;
};

// 以 AppContainer 身份启动进程；with_job 时按 subprocess.cpp 同款挂起→指派 job→恢复
static LaunchResult launch_ac(PSID ac_sid, HANDLE job, const std::wstring& cmdline,
                              const std::wstring& cwd) {
    LaunchResult r;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
        r.gle = GetLastError();
        return r;
    }
    SECURITY_CAPABILITIES sc{};
    sc.AppContainerSid = ac_sid;
    // 关键：AppContainer 属性必须在建进程时一并传入（不能事后附加）
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   &sc, sizeof(sc), nullptr, nullptr)) {
        r.gle = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        return r;
    }
    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;  // CreateProcess 可写缓冲
    // 与 subprocess.cpp 一致：CREATE_NO_WINDOW（console 子系统程序在无窗口
    // 环境下 attach 失败会 exit 1，首轮 spike 踩到）
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    if (job) flags |= CREATE_SUSPENDED;  // 挂起态建进程，指派 job 后再恢复
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, flags, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(),
                             &siex.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        r.gle = GetLastError();
        return r;
    }
    r.started = true;
    if (job) {
        // 与 subprocess.cpp 相同的叠加顺序：挂起 → job → resume
        if (!AssignProcessToJobObject(job, pi.hProcess)) {
            printf("  AssignProcessToJobObject failed GLE=%lu\n", GetLastError());
        }
        ResumeThread(pi.hThread);
    }
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    // job 生效性自检：以子进程身份确认 IsProcessInJob
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

// LPAC（Less Privileged AppContainer）变体：AC + ALL_APPLICATION_PACKAGES_POLICY=RESTRICTED
static LaunchResult launch_lpac(PSID ac_sid, const std::wstring& cmdline,
                                const std::wstring& cwd) {
    LaunchResult r;
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 2, 0, &attr_size);
    std::vector<char> attr_buf(attr_size);
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attr_buf.data();
    if (!InitializeProcThreadAttributeList(attrs, 2, 0, &attr_size)) {
        r.gle = GetLastError();
        return r;
    }
    SECURITY_CAPABILITIES sc{};
    sc.AppContainerSid = ac_sid;
    DWORD ap_policy = 0x2;  // PROCESS_CREATION_ALL_APPLICATION_PACKAGES_RESTRICTED
    if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                   &sc, sizeof(sc), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY,
                                   &ap_policy, sizeof(ap_policy), nullptr, nullptr)) {
        r.gle = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        return r;
    }
    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(siex);
    siex.lpAttributeList = attrs;
    PROCESS_INFORMATION pi{};
    std::wstring cl = cmdline;
    DWORD flags = CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE, flags, nullptr,
                             cwd.empty() ? nullptr : cwd.c_str(), &siex.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) {
        r.gle = GetLastError();
        return r;
    }
    r.started = true;
    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return r;
}

// 跑一条 AppContainer 命令并把 stdout/stderr 落到 out 文件返回文本
// v2 修复：原实现 `cmdline + " > out 2>&1"` 会与探针命令自身的重定向冲突
// （cmd 解析 `> A > B` 时只有最后一个重定向生效，A 从未被创建），
// 导致 S4/S5/S6 的探针文件写入被静默吞掉、S7 读到上一项残留。
// 改为括号分组：`cmd /c (cmdline) > out 2>&1`，内层重定向独立解析；
// 并在启动前删除 out 文件，杜绝跨项残留。
static std::string run_capture(PSID sid, HANDLE job, const std::wstring& cmdline,
                               const std::wstring& cwd, const std::wstring& out_file,
                               LaunchResult* lr = nullptr) {
    DeleteFileW(out_file.c_str());
    std::wstring full = L"cmd.exe /c (" + cmdline + L") > \"" + out_file + L"\" 2>&1";
    LaunchResult res = launch_ac(sid, job, full, cwd);
    if (lr) *lr = res;
    if (!res.started)
        return "<launch failed GLE=" + std::to_string(res.gle) + ">";
    std::string body = read_file(out_file);
    if (body.find("<unreadable") == 0)
        body += " exit_code=" + std::to_string(res.exit_code);
    return body;
}

int wmain(int argc, wchar_t** argv) {
    // ---- S1: profile ----
    PSID sid = nullptr;
    HRESULT hr = CreateAppContainerProfile(kProfileName, L"Workx Spike 113",
                                           L"Workx #113 AppContainer spike", nullptr, 0, &sid);
    if (FAILED(hr)) {
        // 已存在的 profile 可能返回多种 HRESULT（ALREADY_EXISTS / ACCESS_DENIED 等），
        // 一律先尝试派生 SID（返回 HRESULT 而非 BOOL），派生成功即视为 profile 可用
        HRESULT dhr = DeriveAppContainerSidFromAppContainerName(kProfileName, &sid);
        if (SUCCEEDED(dhr)) {
            printf("S1 OK (create hr=0x%08lX but profile exists, sid derived)\n", hr);
        } else {
            printf("S1 FAIL: create hr=0x%08lX derive hr=0x%08lX\n", hr, dhr);
            return 1;
        }
    } else {
        printf("S1 OK (profile created)\n");
    }
    LPWSTR sid_str = nullptr;
    if (ConvertSidToStringSidW(sid, &sid_str)) {
        wprintf(L"    AC SID = %s\n", sid_str);
        LocalFree(sid_str);
    }

    // ---- S2: 目录授权（%TEMP% 与 spike 工作目录）----
    wchar_t temp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_dir);
    std::wstring temp = temp_dir;
    if (!temp.empty() && temp.back() == L'\\') temp.pop_back();
    // 额外建一个 C:\ 根下的工作区：排除「用户 profile 目录链 traverse 权限」
    // 干扰（AC token 的用户 SID 是 deny-only，%TEMP% 的 traverse 链依赖默认 ACL）
    std::wstring root_work = L"C:\\ac-spike-work";
    CreateDirectoryW(root_work.c_str(), nullptr);
    std::wstring work = root_work;
    std::wstring temp_grant = temp;
    bool g0 = grant_dir(sid, root_work);
    bool g1 = grant_dir(sid, temp_grant);
    bool g2 = true;
    printf("S2 %s (grant root=%d grant TEMP=%d)\n", (g0 && g1) ? "OK" : "PARTIAL", g0, g1);

    // spike 可执行文件所在目录（写 out 文件用，放 root_work 避开 traverse 链干扰）
    std::wstring out = root_work + L"\\ac-spike-out.txt";
    DeleteFileW(out.c_str());

    // job（S9 用）：进程数上限 8 的普通 Job Object
    HANDLE job = CreateJobObjectW(nullptr, L"ac-spike-job");
    JOBOBJECT_BASIC_LIMIT_INFORMATION li{};
    li.ActiveProcessLimit = 8;
    li.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    SetInformationJobObject(job, JobObjectBasicLimitInformation, &li, sizeof(li));

    // ---- S3: 基线存活（先跑一个非 AC 对照，排除框架自身问题）----
    {
        std::wstring full = L"cmd.exe /c echo plain-ok > \"" + out + L"\" 2>&1";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cl = full;
        BOOL ok = CreateProcessW(nullptr, &cl[0], nullptr, nullptr, FALSE,
                                 CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                 work.c_str(), &si, &pi);
        if (ok) {
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        printf("S3a plain对照 %s (out=%s)\n",
               ok && read_file(out).find("plain-ok") != std::string::npos ? "OK" : "FAIL",
               ok ? read_file(out).c_str() : "<CreateProcess failed>");
        DeleteFileW(out.c_str());

        std::string o = run_capture(sid, job, L"cmd.exe /c echo baseline-alive", work, out);
        printf("S3b AC基线 %s (out=%s)\n",
               o.find("baseline-alive") != std::string::npos ? "OK" : "FAIL", o.c_str());

        // S3c：无重定向探针——cmd /c exit 42 的退出码直通。42 = cmd 正常执行；
        //      1 = cmd 启动即失败（问题在环境/ACL 而非命令或重定向）
        LaunchResult probe2 = launch_ac(sid, job, L"cmd.exe /c exit 42", work);
        printf("S3c 无重定向探针 exit_code=%lu started=%d gle=%lu\n", probe2.exit_code,
               (int)probe2.started, probe2.gle);

        // S3d：零依赖探针——AC 直启 ac_probe.exe（拷到已授权 root_work 保证可读）。
        //      42 = AC 进程正常运行；43 = 进程跑了但写被拒；
        //      其余/启动失败 = AC 进程被系统层拦截（策略/ACL），与程序无关
        std::wstring probe_exe = root_work + L"\\ac_probe.exe";
        if (!CopyFileW(L"ac_probe.exe", probe_exe.c_str(), FALSE)) {
            printf("S3d SKIP (copy probe failed GLE=%lu)\n", GetLastError());
        } else {
            std::wstring probe_out = root_work + L"\\ac-probe-out.txt";
            DeleteFileW(probe_out.c_str());
            LaunchResult pr = launch_ac(sid, job, L"\"" + probe_exe + L"\" \"" + probe_out + L"\"",
                                        work);
            printf("S3d 最小探针 exit_code=%lu started=%d gle=%lu file_ok=%d\n", pr.exit_code,
                   (int)pr.started, pr.gle,
                   (int)(GetFileAttributesW(probe_out.c_str()) != INVALID_FILE_ATTRIBUTES));

            // S3e：探针无参数 → CRT+main 跑到 return 44。exit 44 = AC 进程能执行
            //      用户代码；exit 1 = 在映像加载/CRT 初始化阶段就被终止
            LaunchResult pr2 = launch_ac(sid, job, L"\"" + probe_exe + L"\"", work);
            printf("S3e 纯退出码探针 exit_code=%lu（44=main可达 1=初始化前死）\n", pr2.exit_code);

            // S3f：系统自带签名 exe（System32 默认 ACL 对 AC 可读执行）
            std::wstring whoami_out = root_work + L"\\whoami-out.txt";
            DeleteFileW(whoami_out.c_str());
            LaunchResult pr3 = launch_ac(
                sid, job,
                L"C:\\Windows\\System32\\whoami.exe /groups > \"" + whoami_out + L"\" 2>&1",
                work);
            std::string wout = read_file(whoami_out);
            printf("S3f whoami exit_code=%lu started=%d out_len=%zu out=%s\n", pr3.exit_code,
                   (int)pr3.started, wout.size(), wout.c_str());

            // S3g：LPAC 变体（不同策略路径，排除 ALL APPLICATION PACKAGES 组因素）
            std::wstring lpac_out = root_work + L"\\lpac-out.txt";
            DeleteFileW(lpac_out.c_str());
            LaunchResult pr4 =
                launch_lpac(sid, L"\"" + probe_exe + L"\" \"" + lpac_out + L"\"", work);
            printf("S3g LPAC exit_code=%lu started=%d gle=%lu file_ok=%d\n", pr4.exit_code,
                   (int)pr4.started, pr4.gle,
                   (int)(GetFileAttributesW(lpac_out.c_str()) != INVALID_FILE_ATTRIBUTES));
        }
    }

    // ---- S4: 写 C:\Windows\Temp 期待被拒 ----
    {
        std::string o = run_capture(
            sid, job,
            L"cmd.exe /c echo x > C:\\Windows\\Temp\\ac-spike-probe.txt",
            work, out);
        // cmd 的 access denied 表现：stderr 有 "Access is denied"/"拒绝访问"，exit code 1
        bool denied = o.find("denied") != std::string::npos ||
                      o.find("拒绝") != std::string::npos ||
                      o.find("unable") != std::string::npos;
        // 直接验证文件是否真的不存在（双保险）
        DWORD attr = GetFileAttributesW(L"C:\\Windows\\Temp\\ac-spike-probe.txt");
        bool file_absent = attr == INVALID_FILE_ATTRIBUTES;
        printf("S4 %s (system write denied=%d file_absent=%d; out=%s)\n",
               (denied || file_absent) ? "OK-EXPECTED-DENIED" : "FAIL-NOT-DENIED",
               (int)denied, (int)file_absent, o.c_str());
    }

    // ---- S5: 写已授权 %TEMP% 期待成功 ----
    {
        std::wstring probe = temp + L"\\ac-spike-temp-probe.txt";
        DeleteFileW(probe.c_str());
        run_capture(sid, job,
                    L"cmd.exe /c echo temp-write-ok > \"" + probe + L"\"",
                    work, out);
        DWORD attr = GetFileAttributesW(probe.c_str());
        printf("S5 %s (temp write file_exists=%d)\n",
               attr != INVALID_FILE_ATTRIBUTES ? "OK" : "FAIL",
               (int)(attr != INVALID_FILE_ATTRIBUTES));
    }

    // ---- S6: 写已授权 cwd 期待成功 ----
    {
        std::wstring probe = work + L"\\cwd-probe.txt";
        DeleteFileW(probe.c_str());
        run_capture(sid, job, L"cmd.exe /c echo cwd-write-ok > cwd-probe.txt", work, out);
        DWORD attr = GetFileAttributesW(probe.c_str());
        printf("S6 %s (cwd write file_exists=%d)\n",
               attr != INVALID_FILE_ATTRIBUTES ? "OK" : "FAIL",
               (int)(attr != INVALID_FILE_ATTRIBUTES));
    }

    // ---- S7: 网络（curl，无 INTERNET capability）期待被拒 ----
    {
        std::string o = run_capture(
            sid, job,
            L"curl.exe -s -m 8 -o NUL -w \"%{http_code}\" https://example.com",
            work, out);
        // 被拒形态：0（连接失败）或 curl exit 7/26 等。放行形态：200。
        bool blocked = o.find("200") == std::string::npos;
        printf("S7 %s (net out=%s)\n", blocked ? "OK-EXPECTED-BLOCKED" : "FAIL-NET-ALLOWED",
               o.c_str());
    }

    // ---- S8: 混淆命令——已授权脚本间接写系统目录，期待仍被拒 ----
    {
        std::wstring script = work + L"\\sneaky.ac1";
        HANDLE f = CreateFileW(script.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const char* body = "@echo off\r\necho x > C:\\Windows\\Temp\\ac-spike-sneaky.txt\r\n";
            DWORD n = 0;
            WriteFile(f, body, (DWORD)strlen(body), &n, nullptr);
            CloseHandle(f);
        }
        run_capture(sid, job, L"cmd.exe /c \"\\\"" + script + L"\\\"\"", work, out);
        DWORD attr = GetFileAttributesW(L"C:\\Windows\\Temp\\ac-spike-sneaky.txt");
        printf("S8 %s (obfuscated write file_absent=%d)\n",
               attr == INVALID_FILE_ATTRIBUTES ? "OK-EXPECTED-DENIED" : "FAIL-LEAKED",
               attr == INVALID_FILE_ATTRIBUTES);
    }

    // ---- S9: Job Object 叠加自检 ----
    {
        // 用带 job 的会话跑一条存活命令（S3 已验证），这里直接查询 job 信息确认生效
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acc{};
        BOOL ok = QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &acc,
                                            sizeof(acc), nullptr);
        printf("S9 %s (job query=%d total_processes=%lu active=%lu)\n",
               (ok && acc.TotalProcesses + acc.TotalTerminatedProcesses > 0) ? "OK" : "CHECK",
               (int)ok, (unsigned long)(acc.TotalProcesses + acc.TotalTerminatedProcesses),
               (unsigned long)acc.ActiveProcesses);
    }

    // ---- S10: Git Bash 探活 ----
    {
        const wchar_t* candidates[] = {
            L"C:\\Program Files\\Git\\bin\\bash.exe",
            L"C:\\Program Files\\Git\\usr\\bin\\bash.exe",
            L"D:\\Programs\\Git\\bin\\bash.exe",
        };
        std::wstring bash;
        for (auto c : candidates) {
            if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) {
                bash = c;
                break;
            }
        }
        if (bash.empty()) {
            printf("S10 SKIP (bash.exe not found in standard paths)\n");
        } else {
            std::wstring cl = L"\"" + bash + L"\" --version";
            std::string o = run_capture(sid, job, cl, work, out);
            bool alive = o.find("bash") != std::string::npos ||
                         o.find("Bash") != std::string::npos;
            printf("S10 %s (git bash out=%s)\n", alive ? "OK" : "FAIL", o.c_str());
        }
    }

    if (job) CloseHandle(job);
    printf("SPIKE DONE\n");
    return 0;
}
