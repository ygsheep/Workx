# 设计：Issue #84 Windows 沙箱退化为 none

> 代码基线：develop `edd2608` · 编写日期：2026-10-01
> 标签：`bug` / `P1`
> 状态：**方案 A 已落地**（PR1，实现与验证见 §2.A.6）；B/C 待排期

---

## 0. 结论先行

Issue 的**事实部分完全成立**：Windows 上没有任何进程级隔离。回代码核实无一处不符。

但核实过程中发现两件 issue 没写、且比原文更重要的事，会直接改变方案排序：

1. **降级状态在 Windows 上根本不可见** —— 比"没有沙箱"更糟。
   `bash_tool.cpp:368` 的上报被 `if (wrapped.was_wrapped)` 门控，而降级路径 `make_degraded()`
   把 `was_wrapped` 置为 `false`（`sandbox_adapter.cpp:398`）→ **永远进不去这个分支**。
   后台执行路径（`bash_tool.cpp:434`）更是**连上报语句都没有**。
   同时 `EventType::SecuritySandboxDegraded` / `SecuritySandboxDisabled`（`audit_logger.h:51-52`）
   **只出现在枚举定义与 `to_string()` 里，全仓零发射点**（grep 全仓确认）——是死代码。
   → issue 建议 3「至少做行为对齐」其实**这一步也没做到**，它应该是**第一优先级**，不是"至少"。

2. **issue 建议 1 的技术描述有误，不能照做。**
   原文写「Job Object … 配合受限令牌降权，阻断对敏感路径的写入」。
   Job Object 的能力边界是 **CPU / 内存 / 进程数 / UI 限制 / 进程树连带终止**，
   它**不具备文件系统与网络的路径级控制能力**；`CreateRestrictedToken` 降权在普通用户会话下
   也无法实现"按路径拒绝写入"。Windows 上能按路径拦 FS / 关网络的机制是 **AppContainer**
   （能力 SID + 目录 ACL）或用 WSL / 容器类重方案。
   → 建议 1 必须拆成 **B（Job Object，拿资源与进程树收益）** 与 **C（AppContainer，拿策略强制收益）** 两件事。

**推荐**：分三个可独立合并的里程碑 A → B → C，其中 **A 立刻做**（纯可见性、零执行行为变更），
B 做进程树连带终止 + 资源上限，C 先做 spike 再决定是否实现。

---

## 1. 已核实的事实

### 1.1 三个缺口（逐个给证据）

| # | 缺口 | 证据 | 结论 |
|---|---|---|---|
| G1 | Windows 无任何进程级后端 | `sandbox_adapter.cpp:336-362` `wrap_with_seatbelt`（`#if defined(__APPLE__)` else `make_degraded`）、`:364-390` `wrap_with_bubblewrap`（`#if defined(__linux__)` else `make_degraded`）、`:328-334` `is_enabled()` 非 Apple/Linux 直接 `return false`；`sandbox_detector.h:31-35` `Backend` 枚举只有 `None/Seatbelt/Bubblewrap` | **成立** |
| G2 | 降级状态不可见 | `bash_tool.cpp:367-371` 上报被 `was_wrapped` 门控；`sandbox_adapter.cpp:392-402` 降级时 `was_wrapped=false` | **新发现** |
| G3 | 审计事件是死代码 | `audit_logger.h:51-52` 定义 + `audit_logger.cpp:48-51` 映射，**全仓无发射点** | **新发现** |

补充：`SandboxAdapter::is_enabled()` 在 src/ 下**没有任何生产调用方**（只有 `test_sandbox.cpp:232`）。
即 BashTool 从不咨询"沙箱是否可用"，一律调 `wrap_command()` 后依赖 `degraded` 标志——
这让 G2 的后果被放大：**唯一**的可见性通道就是那行被门控住的 `report_progress`。

### 1.2 现有测试已把降级"固化"下来（改设计时要注意）

`tests/unit/core/process/test_sandbox.cpp` 已有：

- `test_sandbox.cpp:202` `SandboxAdapter restrictive config degrades on Windows` —— Windows 分支断言
  `was_wrapped == false` / `degraded == true` / `backend_name == "none"`
- `test_sandbox.cpp:232` `SandboxAdapter is_enabled returns false on Windows`

→ 降级是**有意设计**（"优雅降级，不阻断业务"，见 `sandbox/README.md:12`），不是漏写。
**因此方案 A 必须只改"上报门控"，不动 `make_degraded()` 的 `was_wrapped=false` 语义**，
否则会连带打翻这两条用例。这是本次设计里最容易误伤的地方。

### 1.3 进程创建点（决定 Job Object 的接入面）

| 位置 | 用途 |
|---|---|
| `subprocess.cpp:291` `exec_windows()`，`CreateProcessW` 在 `:326` | BashTool / PowerShellTool 主路径 |
| `subprocess.cpp:~743` | `exec_interactive()`（`/edit` 拉起 nvim 等） |
| `mcp_stdio_process.cpp:275` | MCP stdio 服务器 |
| `app.cpp:1526` | TUI 内嵌进程 |

**顺带一个净收益**：超时/取消目前用 `TerminateProcess`（`subprocess.cpp:353`、`:361`），
它**只终止直接子进程**。`cmd.exe /c <命令>` 派生的孙进程会残留。
`TerminateJobObject` 正好补这个洞——这是独立于"沙箱"名号的真实缺陷修复。

### 1.4 架构约束（决定方案形态，最关键的一条）

`SandboxAdapter::wrap_command(cmd, args, config) -> WrappedCommand{cmd, args, was_wrapped, degraded, backend_name}`
是一个**纯字符串重写**接口。

而 Job Object / AppContainer / 受限令牌**都无法用"改写命令行"表达**：它们要求在建进程时参与
（`CREATE_SUSPENDED` → `AssignProcessToJobObject` → `ResumeThread`，或 `STARTUPINFOEX` +
`PROC_THREAD_ATTRIBUTE_JOB_LIST` / `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`）。

→ **方案必须扩展接口**：让 `SandboxAdapter` 继续做"策略来源"（它已经持有 `SandboxConfig`），
把"执行机制"下沉到 process 层。建议在 `WrappedCommand` 旁边增一个平台中立的
`std::optional<ProcessIsolationSpec>` 字段（**不要塞进 `args`**），由 `process::exec()` 消费。

### 1.5 策略数据模型已经够用

`SandboxConfig`（`sandbox_config.h:34-68`）已备好
`allow_write / deny_write / allow_read / deny_read / allow_domains / deny_domains / network_isolated`
+ `restrictive()` / `permissive()` 工厂 + `is_permissive()`。
→ **不需要新造策略模型**，缺的只是 Windows 侧的"解释器"。

---

## 2. 方案

### 方案 A — 让「没开」这件事可见（P1，低风险，约 1 人日）

**零执行行为变更**，只补可见性与留痕。

1. 修上报门控（`bash_tool.cpp:367-371`）：由 `if (wrapped.was_wrapped)` 改为
   `if (wrapped.was_wrapped || wrapped.degraded)`，且 `degraded` 时用 **warn 级**文案，
   形如 `Sandbox: degraded (backend: none — Windows 无进程级隔离)`。
2. 后台路径（`bash_tool.cpp:419-436`）补同样的上报（当前完全没有）。
3. 发射审计事件：`degraded` 时发 `EventType::SecuritySandboxDegraded`（复用现有枚举，
   把 G3 的死代码激活）；`permissive` / `disable_sandbox` 时发 `SecuritySandboxDisabled`。
   建议**进程内只发一次**（首次降级时），避免每条命令刷爆审计日志。
4. `SecuritySandboxDegraded` 的 payload 带上 `platform` / `requested_level`（restrictive|permissive）/
   `actual_backend`（none），便于事后判断"用户以为开了什么"。
5. 顺手清理 `SandboxAdapter::is_enabled()`：它当前无生产调用方。二选一——
   要么在入口（会话启动）用它发一条"当前平台无沙箱"的提示，要么在头文件注明"仅 POSIX 有意义"。

**收益**：直接消除"用户以为开了 restrictive，实际裸跑且无任何提示"。
**风险**：极低。不动 `make_degraded()`，§1.2 的既有用例保持绿。

### A.6 实现记录（PR1）

落地形态与设计有两处偏差，理由如下：

1. **没有直接把回调塞回 `bash_tool.cpp` 的原地 `if`，而是抽成独立模块**
   `src/agent/tool/ShellTool/sandbox_visibility.{h,cpp}`：三个纯函数
   （`classify_sandbox` / `sandbox_progress_message` / `sandbox_audit_detail`）+ 一个
   有状态上报器 `SandboxVisibility`。
   原因：可见性逻辑如果继续内联在 `execute_sync` 里，就只能靠"真跑一条命令"来验证；
   抽出来后"三态判定""文案""审计详情""只留痕一次"全都能直接单测。
2. **PowerShellTool 同步修复**（设计原文只点了 BashTool）：它在
   `powershell_tool.cpp:327` 有**一模一样**的 `was_wrapped` 门控，两边一起修才叫修完。
   两工具的后台路径（`execute_background`）此前都**没有任何**沙箱上报，一并补上。

去重粒度取**工具实例级**（`mutable SandboxVisibility` 成员）而非进程级全局：
- 可测（构造实例即可断言"两次调用只留一条审计"），不需要暴露 `reset_for_testing()` 这类测试钩子
- 工具实例 ≈ 一次会话，换会话重新提示比"进程内永不再提"更符合安全提示的意图

审计事件用既有的 `AuditLogger::log_security()`（`SecuritySandboxDegraded` /
`SecuritySandboxDisabled`，此前是 §1.1-G3 里零发射点的死代码），
detail 采用 `platform=… requested_level=… actual_backend=… reason=…` 的 key=value 形式。
⚠️ **遗留**：`log_security()` 把所有 `Security*` 事件固定为 `Severity::Critical`，
而 `audit_logger.h:35` 的注释把"沙盒降级"列为 `Warn`。本次未改（改一个事件的分级会与
其余 13 个 `Security*` 事件不一致，属独立议题），留作 follow-up。

**验证**（`tests/unit/agent/tool/test_sandbox_visibility.cpp`，标签
`[sandbox_visibility][issue84]`）：

| 断言 | 覆盖 |
|---|---|
| 三态判定（含"未包装未降级也未显式关闭 → 按 degraded 上报"） | 纯函数 |
| active 文案与既有格式逐字符一致；degraded/disabled 含 `WITHOUT OS-level isolation` | 纯函数 |
| 审计详情含 `platform=` / `requested_level=` / `actual_backend=` / `reason=` | 纯函数 |
| 连调 3 次 degraded → 进度 3 条、`security.sandbox_degraded` **仅 1 条** | 端到端（读真 audit.jsonl） |
| degraded + disabled 混调 → 各自 1 条，事件带 tool_name / session_id | 端到端 |
| active → 不写任何降级审计 | 端到端 |
| `BashTool::call()` 真跑 `echo` → 进度里出现 `Sandbox: ` | 工具集成 |

### 方案 B — Job Object：资源上限 + 进程树连带终止（P1，约 2–3 人日）

1. `WrappedCommand` 增 `std::optional<ProcessIsolationSpec> isolation`（平台中立描述：
   `max_processes` / `max_memory_bytes` / `kill_tree_on_exit`），由 `SandboxConfig` 派生。
2. `subprocess.cpp` Windows 路径改：`CreateProcessW(..., CREATE_SUSPENDED | CREATE_NO_WINDOW | ...)`
   → `CreateJobObject` + `SetInformationJobObject`（
   `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_PROCESS_MEMORY`）
   → `AssignProcessToJobObject` → `ResumeThread`。
3. 超时/取消由 `TerminateProcess` 改为 `TerminateJobObject`（连带杀全树）。
4. `exec_interactive()` 同理；MCP 子进程单独评估（MCP server 生命周期长，误杀代价高）。

**收益**：a) 不再残留孙进程（真实缺陷）；b) `restrictive` 档在 Windows 上产生**可观测差异**
（ActiveProcessLimit / 内存上限）；c) 为方案 C 备好挂载点。
**必须写明的边界**：B **不提供**文件系统与网络的路径级隔离。**不要**沿用 issue 原文措辞。
**风险**：父进程若已在某个 Job 内且系统为 Win7（不支持嵌套）→ `AssignProcessToJobObject` 失败，
必须**降级并上报**（正好复用方案 A 的通道），不能静默失败。

### 方案 C — AppContainer：真正实现 restrictive（P2，需先 spike）

1. `CreateAppContainerProfile` 建 profile → `STARTUPINFOEX` 传 `PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES`。
2. `network_isolated=true` → **不授予 `internetClient` 能力**（这是 Windows 上唯一能真按策略关网络的机制）。
3. 需要放开的路径用 `icacls <path> /grant <AppContainer SID>:(OI)(CI)(RX|RW)`。

**必须先 spike 的三个问题**（决定 C 是否可行）：

- **Git Bash（msys2）在 AppContainer 里能否正常启动**？`%TEMP%`、`/usr/bin`、DLL 搜索路径都需要授权，
  否则大量工具链直接崩。**这条不通过，C 就没有意义**（README 宣称 BashTool 为主力工具）。
- 授权意味着**永久修改宿主机目录 ACL**，副作用不可忽视，需要明确的清理策略。
- profile 生命周期管理（`DeleteAppContainerProfile`）与并发运行的冲突处理。

→ 建议 **C 独立开 issue**，先做 spike 报告，再决定是否投入。

### 明确不做

- **不实现"基于路径字符串的黑名单拦截"来冒充沙箱。** 那是 `#35`（已关闭）的**命令过滤层**，
  与进程沙箱是**不同层**。混进来会让"档位生效"变成一句谎话——正是本 issue 想消除的那种错觉。

---

## 3. 修完后的档位语义（对齐用）

| SandboxConfig | macOS | Linux | Windows 现状 | +A | +B | +C |
|---|---|---|---|---|---|---|
| `restrictive` | Seatbelt profile 强制 | bwrap 强制 | 直通，**无提示** | 直通 + **显式降级提示/审计** | + 进程/内存上限、杀全树 | FS deny + 断网 强制 |
| `permissive` | 直通 | 直通 | 直通 | 直通 + `sandbox_disabled` 审计 | 同左 | 同左 |
| `is_enabled()` | 探测 | 探测 | 恒 false，无调用方 | 入口提示当前平台能力 | 同左 | 恒 true |

---

## 4. 验收标准（**必须拆分**）

issue 原验收「Windows 上以 restrictive 档执行 `写 C:\Windows\...` 或 `访问外网`，能被拦截」
**只有方案 C 能达成**。若以此为唯一验收，issue 会长期无法关闭。

| 里程碑 | 验收项 | 可自动化 |
|---|---|---|
| A | Windows 跑一条 restrictive 命令 → 进度输出出现 `Sandbox: degraded (backend: none)` | ✅ 用例可断言 `WrappedCommand` 语义 + 审计文件内容 |
| A | 审计 jsonl 出现 `security.sandbox_degraded`，且只出现一次 | ✅ |
| A | macOS/Linux 行为**不变**（既有 `[sandbox]` 用例全绿） | ✅ |
| B | `cmd.exe /c <派生长命孙进程>` 超时后，孙进程不残留（`QueryInformationJobObject` ActiveProcesses == 0） | ⚠️ 需 Windows runner，当前 CI 无 |
| B | `ActiveProcessLimit` 生效（超过上限的派生被拒） | ⚠️ 同上 |
| C | restrictive 下 socket 连接被拒（无 `internetClient`） | ⚠️ 同上 + 依赖 spike |
| C | 写入 `deny_write` 前缀路径被拒 | ⚠️ 同上 |

> ⚠️ **CI 现实（必须先接受）**：`.github/workflows/code-quality.yml` 全部 job 是 `ubuntu-latest`，
> **无 Windows runner**；`release.yml` 只在 `v*.*.*` tag 时跑 `windows-latest`。
> 即 B/C 的验收项**在 PR 阶段无法被 CI 验证**，只能手工执行并把证据贴进 PR。
> 因此 A 尤其重要：它是唯一能被 CI 守住的那一层。

---

## 5. 落地切分（本项目约定：一个主题一个 PR）

| PR | 标题 | 内容 | 关联 |
|---|---|---|---|
| PR1 | `fix(#84): 沙箱降级状态可见化与审计留痕` | 方案 A（**已落地**） | `Refs #84` |
| PR2 | `feat(#84): Windows Job Object 进程树隔离与资源上限` | 方案 B | `Refs #84` |
| Issue | `[安全] Windows AppContainer 沙箱后端（spike 先行）` | 方案 C 的 spike + 实现 | 新 issue |

用 `Refs #84` 而非 `Closes`：单靠 PR1/PR2 无法关闭本 issue（C 未完成）。

### 施工注意（本项目已知坑）

- 新增 `.cpp` 走 glob 自动收集，但 **VS 生成器下 `cmake --build --target <t>` 绕过 `ZERO_CHECK`，
  不会重配**（本次实测），需先显式 `cmake -S . -B build`。
- 新文件需过 CI 两道阻塞门禁：`clang-format 18.1.8`（列宽按**显示宽度** 100、CJK 算 2 列）、
  `lizard`（CCN ≤ 15，函数 ≤ 50 行）。
- `windows.h`：本仓库现有写法是直接 `#include <windows.h>`（`subprocess.cpp:30`，**未**定义
  `WIN32_LEAN_AND_MEAN`）。新增 Job Object 代码建议补 `#define WIN32_LEAN_AND_MEAN`，
  并注意 `min/max` 宏污染。
- 现有 `[sandbox]` 用例（`tests/unit/core/process/test_sandbox.cpp`）是回归底线，A 阶段应保持全绿。
