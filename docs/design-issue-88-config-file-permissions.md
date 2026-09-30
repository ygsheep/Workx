# 设计：Issue #88 config.json 文件权限加固

> 代码基线：develop `edd2608` · 编写日期：2026-10-01
> 标签：`bug` / `P1`
> 状态：**PR1 已实现**（§2.1 落地，见 §5 改动清单）；§2.2 / §2.3 未开始

---

## 0. 结论先行

Issue 描述**成立**，但有两处需要校正，校正后方案会明显变小：

1. **平台分布不对称，别当成一个"全平台泄露"处理。**
   - POSIX 是实打实的可读泄露：本机实测 `~/.workx/config.json` 为 `mode=644`（`-rw-r--r--`），目录 `0755`。
   - Windows 上配置目录是 `%USERPROFILE%\.workx`（`app_config.cpp:389-393`），文件 ACL **继承自用户目录**，实测为
     `SYSTEM:(I)(F)` / `Administrators:(I)(F)` / `young:(I)(F)` —— **没有** `Users` / `Authenticated Users`。
     也就是说 Windows 侧当前"侥幸安全"，属于**依赖父目录 ACL**，而不是**设计保证**。
   - 所以 issue 的验收标准「`ls -l ~/.workx/config.json` 为 `-rw-------`」只适用于 POSIX，Windows 必须换成 DACL 判据。

2. **不要求助系统凭据存储**（issue 建议 3）——那是 3 个平台各一套 API 的独立工程量，塞进一个 P1 bug 会让它永远关不掉。建议拆出独立 issue。

**推荐方案**：在**唯一写入口** `ConfigManager::save_to_file()` 上做单点加固（P1），外加启动校验（P2）。

---

## 1. 已核实的事实（方案的地基）

### 1.1 唯一写入口

| 事实 | 证据 |
|---|---|
| config.json 的写入点是 `ConfigManager::save_to_file()` | `src/core/config/config_manager.cpp:237-269` |
| 实际落盘用 `std::ofstream file(path)`，**零权限设置** | 同上 `:254` |
| 父目录用 `std::filesystem::create_directories()` 创建，同样无权限设置 | 同上 `:251` |
| 调用方只有向导与 TUI 保存回调 | `src/tui/wizard.cpp:346`、`src/tui/main.cpp:440` |
| `AppConfig` 不持有独立保存路径（只有 load 侧） | `src/agent/config/app_config.cpp:357`（`load_from_env`）、`:372`（`load_from_config_file`） |
| `workx_core` 的源是**显式列表**而非 glob | `src/core/CMakeLists.txt:32-46`，新增 `.cpp` 必须手工登记 |
| Windows 需额外链接 `advapi32` | ACL API（`SetNamedSecurityInfoW` 等）不被其它库透传，已在 `src/core/CMakeLists.txt` 加 `if(WIN32) → PRIVATE advapi32` |

→ 加固点收敛为**一个函数**，不需要在 TUI / wizard 各改一遍。

### 1.2 零加固（全仓 `src/` grep）

`chmod` / `0o600` / `0600` / `S_IRUSR` / `SetNamedSecurityInfo` / `SetFileSecurity` / `umask` /
`std::filesystem::permissions` —— **全部零命中**。
唯一命中是 `src/agent/tool/permission_ask.cpp:120-121` 的字符串 `"chmod 777"`（危险命令黑名单），与文件权限无关。

→ issue 的「全仓零命中」结论**属实**。

### 1.3 实测现状（本机，2026-10-01）

```
# POSIX 视图
-rw-r--r--  1 young 197121 1286  config.json     mode=644
drwxr-xr-x                    .workx/            mode=755

# Windows 视图（icacls）
config.json  NT AUTHORITY\SYSTEM:(I)(F)
             BUILTIN\Administrators:(I)(F)
             YOUNG\young:(I)(F)
```

注意：Git Bash 的 `mode=644` 在 Windows 上是从只读属性合成的，**不代表真实 ACL**；
Windows 侧的唯一有效判据是 `icacls` / `GetNamedSecurityInfoW`。三个 ACE 都带 `(I)`（继承），
即**没有任何一条是本文件自己的**——这正是"没有加固"的定义。

### 1.4 关键陷阱：`ofstream` 不会收紧已存在文件的权限

`std::ofstream file(path)` 以 `trunc` 打开**保留原权限**。含义：

> 即使发了修复版本，**老用户的 644 文件在下次 save 后依然是 644**。

→ 修法必须是"写完显式收紧"，不能只改创建路径。这条是本次设计里最容易被漏掉的一点。

### 1.5 配置目录解析（决定风险边界）

`app_config.cpp:384-410`，优先级链：

1. `$WORKX_CONFIG_DIR`（显式指定，**任意路径**）
2. Windows `%USERPROFILE%\.workx` / POSIX `$HOME/.workx`
3. Windows `%APPDATA%\workx` / POSIX `$XDG_CONFIG_HOME/workx`
4. `<cwd>/.workx`（最后兜底）

第 1 与第 4 条是风险放大器：CI、共享目录、从快捷方式启动时，文件会落在**权限完全不可控**的位置。

### 1.6 已有的"内容侧"缓解（都不解决本 issue）

- `load_from_env()`：环境变量覆盖（`app_config.cpp:357`）
- `redact_input()` / `secret_scanner.cpp`：落盘前内容脱敏与拦截
- `#60`（已关闭）：工具越界读取路径

→ 这些做的是**内容**，本 issue 要的是**静态文件权限**，两者不重叠。issue 的这条判断准确。

---

## 2. 方案

### 2.1 P1 — 单点加固（推荐先做，1 人日）

新增 `src/core/utils/file_permissions.h` / `.cpp`（`src/core/utils/` 现有 error / file_index / line_diff /
path_encoder / result / uuid，无平台权限抽象，属新增而非改造）：

```cpp
namespace agent {  // 与本目录 error.h / result_v2.h / line_diff.h 一致
                   // （path_encoder.h / uuid.h 用 core::util，两种命名并存；
                   //  本模块要返回 agent::ResultV2，故选 agent 免跨命名空间转换）

/// 收紧为「仅属主可读写」（POSIX 0600 / Windows 属主 + SYSTEM）
ResultV2<void> harden_private_file(const std::filesystem::path& path);

/// 收紧为「仅属主可访问」（POSIX 0700 / Windows 属主 + SYSTEM），不递归
ResultV2<void> harden_private_dir(const std::filesystem::path& path);

}  // namespace agent
```

`check_private_scope()`（原设计写在这一节）移到 §2.2 的 PR2 一起做——PR1 里它没有调用方，
提前声明就是死代码。

**POSIX 实现**：`::chmod(path.c_str(), 0600 / 0700)`。失败**不静默**，返回 `err`。

**Windows 实现**（两个必须的细节，否则等于没做）：

1. `GetNamedSecurityInfoW` 读出 → 重建只含 `当前用户` + `NT AUTHORITY\SYSTEM` 的 DACL →
   `SetNamedSecurityInfoW(..., DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION)`。
2. `PROTECTED_DACL_SECURITY_INFORMATION` 是**关键**：没有它，父目录的继承 ACE 会被重新注入，
   加固立刻被撤销（表现为"看起来设置了但 `icacls` 还是那三条 `(I)`"）。

**接入点**（`config_manager.cpp:237-269`）：

```
create_directories(parent)   → 硬化为 0700
ofstream 写 + close()        → 硬化为 0600     ← 顺序必须在 close 之后
```

**失败策略（已定）**：加固失败 → `LOG_WARN` 留痕 + **仍返回配置写入成功**。理由：把"Key 存不下来"
当硬失败会造成比权限更糟的可用性问题（FAT/exFAT、网络盘、只读挂载上 `chmod` 必失败）。

**实现时去掉了原设计的 `security.harden_config_perms` 开关**：给一个安全修复配"关掉它"的开关，
等于把"悄悄失去防护"变成合法配置项，和 #88 要修的是同一类问题。可用性由"失败仅警告"保证即可。

**若将来改成原子写（tmp + rename）**：必须在 `rename` **之前**给临时文件设好权限，
`rename` 会保留源文件权限；写在 rename 之后会出现"权限窗口"。本仓库当前是直写，不涉及。

### 2.2 P2 — 启动校验 + 可选自动收紧（0.5 人日）

在 `load_from_config_file()`（`app_config.cpp:372`）读成功后加一次 `check_private_scope(path)`：

| 平台 | 判为「过宽」的条件 |
|---|---|
| POSIX | `mode & 077` 非零 |
| Windows | DACL 中出现非 inherit-only 的 `BUILTIN\Users` / `Everyone` / `Authenticated Users` |

命中时：`LOG_WARN` 打印当前权限与修复命令；`security.auto_fix_perms=true` 时直接调 `harden_private_file()`。
同时发一条审计事件（`EventType` 已有 `SecuritySensitiveFile`，可直接复用，无需新增枚举）。

### 2.3 P3 — 系统凭据存储（**建议另开 issue，不进 #88**）

Windows Credential Manager / macOS Keychain / Linux Secret Service（libsecret），
config.json 只存 `keyring://workx/default` 引用。

代价：3 个平台各一套 API + 抽象层 + **无 keyring 的环境（CI、无桌面会话的 SSH）必须回退**，
回退路径就是 `$WORKX_API_KEY`。工程量远超一个 P1 bug，且会拖长 #88 的关闭时间。

### 2.4 明确不做

- 不做"把 API Key 加密后仍存在同一文件"的方案：密钥仍要存在本地，等于把问题平移，收益为负。
- 不改配置目录默认位置（如 Windows 从 `%USERPROFILE%\.workx` 改到 `%LOCALAPPDATA%`）：
  会破坏存量用户配置发现，属破坏性变更，不在本 issue 范围。

---

## 3. 验收标准（修正版）

| # | 场景 | 判据 | 可自动化 |
|---|---|---|---|
| A1 | POSIX 新建配置 | `stat -c %a ~/.workx/config.json` == `600`，目录 == `700` | ✅ Linux CI |
| A2 | POSIX 升级已有 644 文件 | 再次 save 后变 `600`（覆盖 §1.4 陷阱） | ✅ Linux CI |
| A3 | Windows 新建配置 | `icacls` 输出**无** `(I)`，且仅属主 + `SYSTEM` | ❌ 本机手工 |
| A4 | Windows 非属主读取被拒 | 需第二账户验证 | ❌ 手工（issue 原验收项） |
| A5 | 加固失败 | 返回 err + 审计留痕，配置仍写入 | ✅ Linux CI（可注入失败） |

> ⚠️ **CI 现实**：`.github/workflows/code-quality.yml` 全部 job 是 `ubuntu-latest`，
> 无 Windows runner；`release.yml` 只在 `v*.*.*` tag 上跑 windows-latest。
> 即 **A3/A4 在 PR 阶段无法被 CI 验证**，必须手工执行并把 `icacls` 输出贴进 PR 描述。
> 这一点要在实现 PR 里写清楚，否则 Windows 分支等于零覆盖。
>
> 补充（实现期发现）：CI 连 `ubuntu-latest` 的 **build/test job 都没有**（只有 lint/复杂度），
> 所以 A1/A2 在 CI 上也不会自动跑。本次改用下面的方式取得跨平台证据。

### 3.1 本次实现的实际验证记录（2026-10-01）

| 平台 | 方式 | 结果 |
|---|---|---|
| Windows (MSVC 14.51, Debug) | `core_unit_tests.exe "[issue88]"` | **5 用例 / 22 断言全绿**；全量 core **187 用例 / 786 断言全绿** |
| Linux (WSL Ubuntu 24.04, g++ 13.3) | 用本地 vcpkg 的 Catch2 3.14 合并版源码编译**真实测试文件** | **5 用例 / 14 断言全绿** |

> 为什么 Linux 侧要单独搭台：本机只有 MSVC，POSIX 分支（`#else` 那段）在 Windows 构建里
> **根本不会被编译**。实测它真的有 bug（见下），所以这一步不是形式主义。

**变异测试（确认断言不是"永远通过"的假守卫）**：

| 变异 | 预期 | 实测 |
|---|---|---|
| M1：去掉 `PROTECTED_DACL_SECURITY_INFORMATION` | DACL 加固失效 | ✅ 8 条断言失败；且复现了 §2.1 预言的陷阱——`AceCount` 由 2 变回 **4**（继承项回流） |
| M2：把 SYSTEM SID 换成 `WinWorldSid`(Everyone) | 出现宽授权 | ✅ 5 条断言失败（`has_local_system` 与 `has_broad_ace` 均触发） |
| M3：POSIX 侧 `0600` 改成 `0644` | 权限未收紧 | ✅ 3 条断言失败（`420 != 384`） |

**由此抓到并修掉的真实 bug**：测试里"模拟历史 0644 文件"原本用
`perms::owner_all | group_read | others_read`，而 `owner_all` **含 `owner_exec`**（=0700），
实际造出的是 **0744**，那条 `REQUIRE(mode_bits(file) == 0644)` 在 POSIX 上必然失败。
Windows 构建里这段在 `#else` 中，从未被编译，所以本地一直没暴露。

**当前未覆盖**：A4（非属主读取被拒）需第二账户，仍为手工项；A5（加固失败留痕）未写用例。

---

## 4. 测试方案

- 新增 `tests/unit/core/utils/test_file_permissions.cpp`，标签 `[file_permissions][issue88]`
  （`tests/unit/core/utils/` 已存在，内含 `test_error.cpp` / `test_file_index.cpp` 等，无需新建目录）
- 用 `std::filesystem::temp_directory_path()` 造临时文件（**不要**写真实 `~/.workx`，避免污染用户配置）
- POSIX 断言：`std::filesystem::status(path).permissions()`
- Windows 断言：`GetNamedSecurityInfoW` 读回 DACL，断言 `AceCount == 2` + `SE_DACL_PROTECTED`
  + 含 `LocalSystem`（三条合起来足以证伪"加固是空操作"）
- 回归加固：原计划写进 `test_config_manager.cpp`，实现时改为**同文件内**加一条 `save_to_file`
  端到端用例（权限断言与 `inspect_dacl` helper 都在本文件，跨文件复用反而要重复一份）
- 运行：`core_unit_tests` **本 worktree 此前从未构建过**（只构建过 `agent_unit_tests`），
  首次构建要额外编译 `tests/unit/core/**` 的 16 个源文件

### 施工注意（本项目已知坑）

- 新增 `.cpp` 由 glob 自动收集，但 **VS 生成器下 `cmake --build --target <t>` 会绕过 `ZERO_CHECK`，
  不会重新配置**（本次实测），需先显式 `cmake -S . -B build` 再 build。
- 新文件必须过 CI 两道阻塞门禁：`clang-format 18.1.8`（列宽按**显示宽度** 100、CJK 算 2 列）与
  `lizard`（CCN ≤ 15，函数 ≤ 50 行）。
- `windows.h` 在本仓库现有写法是直接 `#include <windows.h>`（`subprocess.cpp:30`），
  新增实现建议先 `#define WIN32_LEAN_AND_MEAN` 以免污染；不要用 `windows.h` 里的 `min/max` 宏。
- 文件命名为 `file_permissions.h/.cpp`，勿与 `src/agent/tool/permission_ask.cpp`（权限询问）混淆。

### 在 Windows 上验证 POSIX 分支（本次新增的可复用套路）

本机只有 MSVC，`#if !defined(_WIN32)` 那段永远编译不到。可用 WSL 补上：

1. 源码取自本地 vcpkg 构建目录的 Catch2 合并版：
   `D:/Programs/vcpkg/buildtrees/catch2/src/v<与已安装版本一致>.clean/extras/`（本次 v3.14.0）
2. 造一个薄 shim 目录 `catch2/catch_test_macros.hpp` → `#include "catch_amalgamated.hpp"`，
   并把它放在 `-I` **最前**（否则会命中 vcpkg 的 Catch2 真头文件，进而在链接期找不到库）
3. 编译命令（合并版自带 `main()`，无需 `tests/test_main.cpp`）：
   ```
   g++ -std=c++20 -I <shim> -I src -I lib -I build/vcpkg_installed/x64-windows/include -I <extras> \
       tests/.../test_file_permissions.cpp src/core/utils/file_permissions.cpp \
       src/core/utils/error.cpp src/core/config/config_manager.cpp lib/liblogger/logger.cpp \
       <extras>/catch_amalgamated.cpp -o /tmp/real_perm_test
   ```
4. 链接期缺 `Error::to_string()` 就补 `src/core/utils/error.cpp`（实测只缺这一个）

> WSL 命令要从 PowerShell 工具调用；走 Git Bash 会把 `/mnt/c/...` 错拼成
> `C:/Users/.../PortableGit/versions/.../mnt/c/...`（MSYS 路径转换）。

---

## 5. 落地切分

- **PR1** `fix(#88): config.json 写入后收紧文件权限`（§2.1）— `Refs #88` — **已实现**
- **PR2** `feat(#88): 配置权限启动校验与自动收紧`（§2.2）— `Refs #88` — 未开始
- **Issue** `[安全] API Key 迁移至系统凭据存储`（§2.3）— 独立开

> 用 `Refs #88` 而非 `Closes`：单个 PR 不足以关闭 issue（跨平台验收 A3/A4 需手工确认）。

### PR1 实际改动清单

| 文件 | 动作 |
|---|---|
| `src/core/utils/file_permissions.h` | 新增 |
| `src/core/utils/file_permissions.cpp` | 新增（POSIX chmod / Windows DACL 双实现） |
| `src/core/CMakeLists.txt` | 登记新源 + `if(WIN32) → advapi32` |
| `src/core/config/config_manager.cpp` | `save_to_file()` 写后收紧；失败仅 `LOG_WARN` |
| `tests/unit/core/utils/test_file_permissions.cpp` | 新增 5 条用例（含 `save_to_file` 端到端） |
