# BashTool — Shell 执行工具

> 执行 shell 命令并返回 stdout / stderr / exit code，支持同步执行、后台任务、沙盒包装、超时控制与进度回调。
>
> 对标 Claude Code CLI 的 `BashTool`，在 WorkX 的 ReAct 架构下作为通用 Shell 入口集成。
>
> 版本：v1.1.0

---

## 一、概述

`BashTool` 是 Agent 工具集中负责"执行 Shell 命令"的工具，对应 LLM 工具名 `Bash`。支持：

- **同步执行（默认）**：阻塞等待命令完成，返回 stdout / stderr / exit_code
- **后台执行**：`run_in_background=true` 时通过 `TaskManager` 异步执行，立即返回 task_id
- **沙盒包装**：默认通过 `SandboxAdapter::wrap_command()` 包装命令（可禁用）
- **超时控制**：默认 120 秒，最大 600 秒
- **进度回调**：通过 `ctx.progress_callback` 上报执行进度
- **取消信号**：绑定 `ctx.cancel_flag`，外部可中断执行

### 文件清单

| 文件 | 用途 | 版本 |
|------|------|------|
| [bash_tool.h](bash_tool.h) | 工具接口声明（含同步/后台私有方法） | v1.1.0 |
| [bash_tool.cpp](bash_tool.cpp) | 工具实现（call 分发 + 同步/后台路径 + 输出格式化） | v1.1.0 |
| [README.md](README.md) | 本文档 | v1.1.0 |

### 依赖的内部模块

| 模块 | 用途 |
|------|------|
| [core/process/subprocess.h](../../core/process/subprocess.h) | 跨平台子进程执行（`process::exec()`） |
| [core/process/exec_output.h](../../core/process/exec_output.h) | `ExecOutput` / `ExecOptions` 类型 |
| [core/process/sandbox/sandbox_adapter.h](../../core/process/sandbox/sandbox_adapter.h) | 沙盒命令包装 |
| [core/process/sandbox/sandbox_config.h](../../core/process/sandbox/sandbox_config.h) | `restrictive()` / `permissive()` 配置 |
| [core/task/task_manager.h](../../core/task/task_manager.h) | `ITaskManager::launch()` 后台任务 |
| [agent/tool/context.h](../context.h) | `ToolContext`：cwd / cancel_flag / task_manager / progress_callback |

---

## 二、工具元数据

| 字段 | 值 |
|------|-----|
| name | `Bash` |
| description | Executes a shell command and returns stdout, stderr, and exit code. Supports timeout, working directory, and optional background execution. |
| namespace | `agent::tool` |
| 基类 | `ITool` |
| 同步/异步 | 同步返回 `ResultV2<ToolResult>`（后台模式立即返回 task 信息） |

---

## 三、输入 Schema

```json
{
  "type": "object",
  "properties": {
    "command":                       { "type": "string",  "description": "The shell command to execute" },
    "description":                   { "type": "string",  "description": "Brief description of what the command does (5-10 words)" },
    "timeout":                       { "type": "integer", "description": "Timeout in milliseconds (max 600000)", "default": 120000 },
    "run_in_background":             { "type": "boolean", "description": "Run command in background, return immediately with task id", "default": false },
    "dangerously_disable_sandbox":   { "type": "boolean", "description": "Disable sandbox restrictions for this command", "default": false }
  },
  "required": ["command"],
  "additionalProperties": false
}
```

### 字段说明

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `command` | string | 是 | — | Shell 命令文本（不允许空字符串） |
| `description` | string | 否 | — | 命令简述（5-10 词，LLM 自填，未强校验） |
| `timeout` | int | 否 | `120000` (120s) | 超时毫秒数，超过 `600000` 自动截断；`<=0` 回退默认值 |
| `run_in_background` | bool | 否 | `false` | `true` 走后台路径，立即返回 task_id |
| `dangerously_disable_sandbox` | bool | 否 | `false` | `true` 使用 `SandboxConfig::permissive()` |

> `additionalProperties: false` 对齐其他工具 schema 严格策略，禁止额外字段。

---

## 四、输出格式

### 同步执行

LLM 可读文本，由 `format_result()` 拼装：

```
<error>Command exited with code N</error>    # 仅 exit_code != 0 / timed_out / cancelled 时
<stdout>
... 命令标准输出（已 strip_empty_lines） ...
</stdout>
<stderr>
... 命令标准错误（已 strip_empty_lines） ...
</stderr>
```

- 三种状态标签：`<error>Command timed out</error>` / `<error>Command was cancelled</error>` / `<error>Command exited with code N</error>`
- stdout / stderr 分别以 `<stdout>...</stdout>` / `<stderr>...</stderr>` 包裹
- 两者皆空时输出 `(no output)\n`
- 输出超过 `8000` 字符（`kMaxOutputChars`）按头尾各半截断，中间插入 `... [output truncated, N characters omitted] ...`

### 后台执行

立即返回任务信息文本，不阻塞：

```
Command running in background with ID: bash:<command 前缀>
Command: <原始 command>
Output will be available when the task completes.
```

任务完成时由 UI 层（`ChatRenderer`）订阅 `TaskCompletedEvent` / `TaskFailedEvent` 显示通知，不向 LLM 回送输出。

---

## 五、执行管道

### 5.1 call() 入口分发

```
call(input, ctx)
    │
    ▼
1. 参数解析与校验
   ├─ command 缺失/非字符串 → err(MissingArgument)
   ├─ command 为空          → err(InvalidInput)
   ├─ timeout 钳制（<=0 → 默认；>600000 → 600000）
   └─ cwd 解析（参数 > ctx.cwd）
    │
    ▼
2. 取消预检
   └─ ctx.is_cancelled() → err(Cancelled)
    │
    ▼
3. 分发
   ├─ run_in_background=true  → execute_background()
   └─ 否则                    → execute_sync()
```

### 5.2 execute_sync 同步路径

```
execute_sync(command, cwd, timeout_ms, disable_sandbox, ctx)
    │
    ▼
1. 上报进度 "Executing: <command>"
    │
    ▼
2. 构建 SandboxConfig
   ├─ disable_sandbox=true → permissive()
   └─ 否则                 → restrictive(cwd)
    │
    ▼
3. SandboxAdapter::wrap_command(kShell, {kShellFlag, command}, sb_config)
   ├─ Windows: cmd.exe /c "<command>"
   └─ POSIX:   /bin/sh -c "<command>"
    │
    ▼
4. 上报沙盒状态（#84：active / degraded / disabled，全部逐次上报；
   降级与显式关闭另各写一条审计事件，见 6.2 与 6.5）
    │
    ▼
5. 构建 ExecOptions
   ├─ cwd / args / timeout
   └─ is_cancelled = [flag]() { flag->load(acquire); }   # 绑定 ctx.cancel_flag
    │
    ▼
6. process::exec(wrapped.cmd, opts)
   ├─ 启动失败 → err(err.code, "Failed to execute command: ...")
   └─ 成功     → 继续
    │
    ▼
7. format_result(out) + truncate_output(8000)
    │
    ▼
8. 上报完成状态
   ├─ success   → "Command completed successfully"
   ├─ timed_out → "Command timed out"
   ├─ cancelled → "Command cancelled"
   └─ 非零退出  → "Command exited with code N"
    │
    ▼
9. 返回 ToolResult::ok(formatted)
```

### 5.3 execute_background 后台路径

```
execute_background(command, cwd, timeout_ms, disable_sandbox, ctx)
    │
    ▼
1. ctx.task_manager_ptr 非空检查
   └─ nullptr → err(ConfigInvalid, "requires TaskManager")
    │
    ▼
2. 构建 SandboxConfig + wrap_command（同同步路径）
    │
    ▼
3. task_name = "bash:" + command.substr(0, 40)
    │
    ▼
4. tm.launch(task_name, lambda, TaskType::Background)
   ├─ lambda 捕获 command/cwd/timeout_ms/wrapped
   ├─ 内部构建 ExecOptions，is_cancelled 绑定 should_cancel
   ├─ 调用 process::exec()（输出当前实现丢弃，后续迭代扩展）
   └─ TaskManager 通过 EventBus 发布 TaskCompletedEvent / TaskFailedEvent
    │
    ▼
5. task 为空 → err(InternalError, "failed to launch background task")
    │
    ▼
6. 返回 ToolResult::ok("Command running in background with ID: ...")
   ├─ 上报进度 "Background task started: <task_name>"
   └─ UI 层（ChatRenderer）订阅事件并显示完成/失败通知
```

---

## 六、关键特性

### 6.1 跨平台 Shell 选择

```cpp
#ifdef _WIN32
constexpr const char* kShell = "cmd.exe";
constexpr const char* kShellFlag = "/c";
#else
constexpr const char* kShell = "/bin/sh";
constexpr const char* kShellFlag = "-c";
#endif
```

通过 shell 执行命令，使管道 `|`、重定向 `>`、复合命令 `&&` / `;` 均可用。

### 6.2 沙盒包装

默认启用 `SandboxConfig::restrictive(cwd)`：

- 限制文件系统访问到 `cwd` 及必要临时目录
- 限制网络访问（平台相关：Linux bwrap / macOS sandbox-exec / Windows Job Object）
- `dangerously_disable_sandbox=true` 切换为 `permissive()`，仅用于明确需要无限制访问的场景
- 沙盒不可用时 `SandboxAdapter::wrap_command()` 自动降级（`wrapped.degraded=true`），仍执行命令但**如实上报**降级状态
- #84：降级上报不再被 `was_wrapped` 门控 —— 同步与后台路径都会调用
  `SandboxVisibility::report()`，Windows 上不再出现"以为开了 restrictive、实际裸跑且零提示"
- 审计留痕：`degraded` 写 `security.sandbox_degraded`，`dangerously_disable_sandbox` 写
  `security.sandbox_disabled`；两类各在工具实例内只写一次，避免逐条命令刷爆 `audit.jsonl`

### 6.3 超时控制

| 常量 | 值 | 说明 |
|------|-----|------|
| `kDefaultTimeoutMs` | `120'000` (120s) | 默认超时，对齐 cc BashTool |
| `kMaxTimeoutMs` | `600'000` (600s) | 最大上限，对齐 cc `getMaxTimeoutMs` |

- `timeout <= 0` 回退默认值
- `timeout > 600000` 截断为 600000
- 超时后 `process::exec()` 返回 `timed_out=true`，格式化为 `<error>Command timed out</error>`

### 6.4 取消机制

`ToolContext::cancel_flag`（`const std::atomic<bool>*`）由 ReActLoop 注入，BashTool 将其绑定到 `ExecOptions::is_cancelled` 回调：

```cpp
if (ctx.cancel_flag != nullptr) {
    const std::atomic<bool>* flag = ctx.cancel_flag;
    opts.is_cancelled = [flag]() {
        return flag->load(std::memory_order_acquire);
    };
}
```

- `subprocess::exec()` 在执行循环中轮询 `is_cancelled`，置位后终止子进程
- 入口处也做预检：`ctx.is_cancelled()` 为真时立即返回 `Cancelled`
- 后台任务的 lambda 内部捕获 `should_cancel`（TaskManager 提供），同样绑定到 `is_cancelled`

### 6.5 进度回调

`ctx.report_progress(text)` 在关键节点上报进度，由 ReActLoop 注入的 `progress_callback` 转发到 UI：

| 时机 | 文本 |
|------|------|
| 同步执行开始 | `Executing: <command>` |
| 沙盒状态 | `Sandbox: <active\|degraded\|disabled> (backend: <name>)`（#84：三态全部上报，degraded/disabled 附带 `command runs WITHOUT OS-level isolation`） |
| 同步完成（成功） | `Command completed successfully` |
| 同步完成（超时） | `Command timed out` |
| 同步完成（取消） | `Command cancelled` |
| 同步完成（非零退出） | `Command exited with code N` |
| 后台启动 | `Background task started: <task_name>` |

### 6.6 后台任务与 UI 通知

后台执行路径通过 `TaskManager::launch(TaskType::Background)` 异步执行，主线程立即返回。任务生命周期事件通过 `EventBus` 发布：

| 事件 | 触发时机 | UI 行为 |
|------|---------|--------|
| `TaskCompletedEvent` | lambda 执行完毕 | `ChatRenderer` 过滤 `bash:` 前缀，绿色输出 `[bg] <task_name> completed (<duration>ms)` |
| `TaskFailedEvent` | lambda 抛异常 | `ChatRenderer` 红色输出 `[bg] <task_name> failed: <error_message>` |

> **当前限制**：后台任务输出（stdout/stderr）当前实现丢弃，仅通知完成状态。后续迭代可扩展 `Task` 结构以保存输出，供 LLM 通过新工具查询。

### 6.7 输出截断

```cpp
constexpr size_t kMaxOutputChars = 8'000;
```

超过阈值按头尾各半截断，与 `ToolExecutor::MAX_TOOL_RESULT_LENGTH` 对齐，避免长输出刷屏 LLM 上下文。截断后中间插入：

```
... [output truncated, N characters omitted] ...
```

### 6.8 空行清理

`strip_empty_lines()` 压缩连续空行为单行，对齐 cc `stripEmptyLines`，减少输出噪声：

- 仅含空白字符的行视为空行
- 连续多个空行合并为 1 个
- 不删除首尾空行（保留命令输出原貌）

---

## 七、错误处理

所有错误通过 `ResultV2<ToolResult>::err()` 返回，不抛异常。

| 错误场景 | 错误码 | 错误信息 |
|---------|--------|---------|
| `command` 缺失或非字符串 | `MissingArgument` | `BashTool: 'command' is required` |
| `command` 为空字符串 | `InvalidInput` | `BashTool: 'command' must not be empty` |
| 执行前已取消 | `Cancelled` | `BashTool: cancelled before execution` |
| 后台模式无 TaskManager | `ConfigInvalid` | `BashTool: run_in_background=true requires TaskManager (ctx.task_manager_ptr is null)` |
| 后台任务启动失败 | `InternalError` | `BashTool: failed to launch background task` |
| 子进程启动失败 | 透传 `process::exec` 错误码 | `Failed to execute command: <err.message>` |
| 命令非零退出 | —（`ToolResult::ok`） | 文本中含 `<error>Command exited with code N</error>` |
| 命令超时 | —（`ToolResult::ok`） | 文本中含 `<error>Command timed out</error>` |
| 命令被取消 | —（`ToolResult::ok`） | 文本中含 `<error>Command was cancelled</error>` |

> 命令执行失败（非零退出 / 超时 / 取消）不作为工具错误返回，而是包进 `ToolResult::ok` 的文本中，让 LLM 看到 stdout/stderr 后自主决策。

---

## 八、使用示例

### 8.1 同步执行

```cpp
#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/context.h"

using namespace agent::tool;

BashTool tool;
ToolContext ctx;
ctx.cwd = "/home/user/project";
// ctx.cancel_flag / config_manager_ptr / task_manager_ptr 由 ReActLoop 注入

nlohmann::json input = {
    {"command", "ls -la src/"},
    {"description", "list source directory"}
};

auto r = tool.call(input, ctx);
if (r.is_ok()) {
    std::cout << r.value().text;  // 含 <stdout>...</stdout>
}
```

### 8.2 长时任务后台执行

```cpp
nlohmann::json input = {
    {"command", "npm run dev"},
    {"run_in_background", true}
};

auto r = tool.call(input, ctx);
// 立即返回，r.value().text 含 task_id
// UI 在任务完成时显示 "[bg] bash:npm run dev completed"
```

### 8.3 带超时与禁用沙盒

```cpp
nlohmann::json input = {
    {"command", "ping -c 100 example.com"},
    {"timeout", 5000},
    {"dangerously_disable_sandbox", true}  // ping 需要网络访问
};

auto r = tool.call(input, ctx);
// 5 秒后超时，文本含 "<error>Command timed out</error>"
```

### 8.4 进度回调订阅

```cpp
ctx.progress_callback = [](const std::string& text) {
    std::cerr << "[progress] " << text << "\n";
};

nlohmann::json input = {{"command", "echo hello"}};
tool.call(input, ctx);
// 输出：
// [progress] Executing: echo hello
// [progress] Sandbox: active (backend: bwrap)
// [progress] Command completed successfully
```

---

## 九、与 Claude Code 对比

| 特性 | Claude Code | 本工具 (v1.1.0) | 差异说明 |
|------|-------------|-----------------|----------|
| 工具名 | `Bash` | `Bash` | 一致 |
| 同步执行 | ✅ | ✅ | 一致 |
| 后台执行 | ✅ `run_in_background` | ✅ `run_in_background` | 一致 |
| 默认超时 | 120s | 120s | 一致 |
| 最大超时 | 600s | 600s | 一致 |
| 沙盒包装 | ✅ | ✅ | 本工具沙盒由内部 `SandboxAdapter` 提供，cc 走不同方案 |
| 取消信号 | ✅ AbortController | ✅ `ctx.cancel_flag` (atomic) | 机制不同 |
| 进度回调 | ✅ onProgress | ✅ `ctx.progress_callback` | 一致 |
| 输出截断 | ✅ | ✅ 8000 字符 | 一致 |
| 空行清理 | ✅ stripEmptyLines | ✅ strip_empty_lines | 一致 |
| stdout/stderr 分离 | ✅ | ✅ `<stdout>` / `<stderr>` 标签 | 格式不同 |
| 非零退出处理 | 标签包裹 | `<error>exited with code N</error>` | 一致 |
| 图片输出检测/压缩 | ✅ | ❌ | 未实现 |
| sed 编辑预览 | ✅ | ❌ | 未实现 |
| AST 安全解析 | ✅ | ❌ | 未实现 |
| 命令持久化 / 重放 | ✅ | ❌ | 未实现 |
| 跨平台 shell | bash | cmd.exe (Win) / sh (POSIX) | 本工具更跨平台 |

### 设计差异分析

1. **沙盒策略**：cc 的沙盒细节未公开，本工具通过 [SandboxAdapter](../../core/process/sandbox/sandbox_adapter.h) 统一封装 Linux bwrap / macOS sandbox-exec / Windows Job Object，并支持 `restrictive()` / `permissive()` 配置切换。
2. **取消机制**：cc 用 Node.js `AbortController`，本工具用 `std::atomic<bool>*` 指针直接绑定到 `subprocess::exec()` 的轮询回调，无运行时依赖。
3. **后台任务输出**：cc 后台任务输出可查询，本工具 v1.1.0 当前丢弃输出，仅通过事件通知 UI 完成状态，待后续迭代扩展。
4. **shell 选择**：cc 固定 bash，本工具按平台选 `cmd.exe` / `/bin/sh`，原生支持 Windows。
5. **简化项**：暂未实现图片输出检测/压缩、sed 编辑预览、AST 安全解析等高级特性。

---

## 十、测试策略

单元测试见 [tests/unit/agent/tool/test_bash_tool.cpp](../../../../tests/unit/agent/tool/test_bash_tool.cpp)，覆盖：

| 类别 | 用例 | 期望结果 |
|------|------|---------|
| **元信息** | name / description / prompt 非空 | ✅ |
| **元信息** | schema 含 `command` required + 4 个可选字段 | ✅ |
| **参数校验** | 缺失 `command` | err(MissingArgument) |
| **参数校验** | 空 `command` | err(InvalidInput) |
| **参数校验** | 非字符串 `command` | err(MissingArgument) |
| **同步执行** | `echo hello_world` | ok，含 `hello_world` 与 `<stdout>` |
| **同步执行** | `exit 42` | ok，含 `42` 与 `<error>` |
| **同步执行** | `sleep 10` + timeout=200 | ok，含 `timed out` |
| **取消** | `cancel_flag=true` 预检 | err(Cancelled) |
| **后台任务** | `run_in_background=true` + 无 TaskManager | err(ConfigInvalid) |
| **进度回调** | `echo` 触发 ≥2 次回调 | 第一次含 `Executing` |
| **沙盒** | `dangerously_disable_sandbox=true` 不报错 | ok |
| **超时钳制** | `timeout=999999999` 自动截断 | ok |

跨平台命令选择：Windows 用 `cmd.exe /c echo` / `exit /b 42` / `ping -n 10`，POSIX 用 `/bin/sh -c` / `exit 42` / `sleep 10`。

沙箱状态可见性（#84）的用例单独放在
[tests/unit/agent/tool/test_sandbox_visibility.cpp](../../../../tests/unit/agent/tool/test_sandbox_visibility.cpp)，
标签 `[sandbox_visibility][issue84]`：三态判定、进度文案、审计详情、
"连调 3 次只留 1 条审计"、active 不写审计，以及 `BashTool::call()` 的端到端上报。

---

## 十一、设计决策

| 决策 | 理由 |
|------|------|
| 同步返回 `ResultV2<ToolResult>` | 遵循项目约定，与 FileRead/Write/Edit 一致 |
| 后台任务通过 `TaskManager::launch()` | 复用项目既有线程池与事件总线，UI 通过 `TaskCompletedEvent` 自然接入 |
| 默认启用沙盒 | 安全优先，`restrictive()` 限制文件/网络访问；`dangerously_disable_sandbox` 显式命名提示风险 |
| 通过 shell 执行命令（`/bin/sh -c` / `cmd.exe /c`） | 支持管道、重定向、复合命令，对齐 cc 行为 |
| 超时上限 600s | 对齐 cc `getMaxTimeoutMs`，防止 LLM 误设极长超时 |
| 输出截断 8000 字符 | 对齐 `ToolExecutor::MAX_TOOL_RESULT_LENGTH`，避免刷屏 LLM 上下文 |
| 非零退出/超时/取消不作为 err 返回 | 让 LLM 看到 stdout/stderr 自主决策，符合 cc 行为 |
| `additionalProperties: false` | 严格 schema 校验，对齐其他工具 |
| 任务名 `bash:<command 前 40 字符>` | 前缀过滤便于 UI 识别 Bash 后台任务 |
| 后台输出当前丢弃 | v1.1.0 简化实现，后续迭代扩展 Task 结构保存输出 |
| 取消信号用 `const std::atomic<bool>*` | 与 ToolContext 既有契约一致，零开销绑定到 subprocess 回调 |
| 进度文本英文 | 与 prompt() / description() 语言一致，避免 UI 混排 |

---

## 十二、路线图

### v1.1.0（已完成 ✅）

- [x] 同步执行（subprocess + 沙盒包装 + 超时 + 取消）
- [x] 后台执行（TaskManager + EventBus 通知）
- [x] 进度回调
- [x] 输出格式化（stdout/stderr 分离 + 截断 + 空行清理）
- [x] 单元测试（15 个用例）

### v1.2.0（短期）

- [ ] 后台任务输出持久化（扩展 Task 结构保存 stdout/stderr，供 LLM 查询）
- [ ] 新增 `KillShellTool` / `GetBashOutputTool` 配合后台任务管理
- [ ] 进度回调接入 stdout 增量（subprocess onProgress → ctx.progress_callback）

### v1.3.0（中期）

- [ ] AST 安全解析（识别危险命令模式：`rm -rf /`、`dd` 等）
- [ ] 命令持久化与重放（会话级命令历史）
- [ ] 图片输出检测/压缩（base64 图片识别 + 缩略）
- [ ] sed 编辑预览（`sed -i` 前显示 diff）

### v2.0.0（远期）

- [ ] 命令权限白名单（配置驱动，限制可执行命令集合）
- [ ] 跨会话命令历史与回放
- [ ] 后台任务输出流式订阅（WebSocket 推送）
- [ ] 多 shell 支持（PowerShell / fish / zsh 可选）
