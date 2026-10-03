# 设计方案 — Issue #77：缺失 headless / 非交互执行模式

- **Issue**: https://github.com/ygsheep/Workx/issues/77
- **分支**: `fix/issue-77-80-headless-and-schema`（与 #80 合并处理）
- **类型**: enhancement（功能）
- **状态**: 待审核 —— 未编写任何实现代码
- **作者**: workx agent
- **日期**: 2026-09-30

---

## 1. 需求背景与验收标准

### 1.1 背景

Workx 只有 FTXUI 交互式 TUI 入口，**没有任何非交互（headless / print）执行模式**。这导致它无法被脚本、CI 流水线或自动化评测框架（Harbor / SWE-bench / Aider Polyglot）驱动。

### 1.2 证据（issue 原文）

- `src/tui/main.cpp:77-101`：命令行只识别 `--mock` / `--smoke` / `--version`；无 `-p` / `--print` / `--prompt` / `--output-format`。
- 全仓 grep `headless` / `--print` / `non_interactive` / `pipe_mode`：零命中。
- 无 TTY 时（`--smoke` 除外）会走首次运行向导 / TUI 装配。

### 1.3 验收标准

在无 TTY 的 Docker / CI 管道中，`workx -p "<task>" --output-format json` 能完整跑通一条 ReAct 链路，并从 stdout 解析出结构化结果。

---

## 2. 受影响模块定位

| 文件 | 现状 | 改动 |
|------|------|------|
| `src/tui/main.cpp` | 入口仅识别 3 个参数，强依赖 TUI 装配 | 新增参数解析 + headless 分支短路 |
| `src/agent/factory.h/.cpp` | `create_session` 返回 Backend + ChatSession | 复用（headless 也走同一装配） |
| `src/agent/core/chat_session.h/.cpp` | `send_message` 触发后台异步推理，靠事件总线通知 | 需同步等待完成的机制（见 §4） |
| `src/agent/core/react_loop.h/.cpp` | `run()` 返回 `ReActResult`（含统计） | **关键复用点**：可直接同步调用 |
| `src/agent/api/i_completion_provider.h` | provider 接口 | 复用 |
| `src/tui/CMakeLists.txt` | `add_executable(workx ...)` | 可复用同一可执行文件，或新增 `workx_headless` 目标 |

### 2.1 关键复用点

`ReActLoop::run()` 是**同步阻塞**的，返回 `ReActResult`（含 `final_answer` / `total_tool_calls` / token 统计等），这正是 headless 模式需要的。核心难点不是循环本身，而是：

1. 绕过 `ChatSession` 的**异步事件驱动**包装（`send_message` 提交到后台 Task，通过 EventBus 回传结果）；
2. 跳过 TUI / 向导 / 文件索引 / Island 等交互装配。

---

## 3. 资料调研结论

### 3.1 业界标准：Claude Code headless

Claude Code 是 headless 模式的业界参照，语义已形成事实标准：

```bash
claude -p "task"                        # 非交互，打印结果后退出
claude -p "task" --output-format json   # JSON + 元数据（session_id / usage / cost）
claude -p "task" --output-format stream-json  # NDJSON 实时流
claude -p "task" --allowedTools "Read,Edit,Bash(...)"  # 权限白名单
claude -p "task" --permission-mode accept-edits   # 无人值守档
claude -p "task" --json-schema '{...}'  # 强制结构化输出
echo "task" | claude -p                 # 从 stdin 读
```

关键设计要点（可直接对齐）：

1. **`-p/--print`** 触发非交互模式，从参数或 stdin 读任务，写 stdout。
2. **`--output-format`** 三态：`text`（默认）/ `json`（含 session_id、usage、total_cost_usd）/ `stream-json`（NDJSON）。
3. **`--bare`**：跳过 hooks/skills/plugins/MCP 自动发现，保证 CI 确定性。
4. **权限即安全边界**：headless 无人值守，必须靠 `--allowedTools` / `--permission-mode` 预授权，而非运行时 prompt。
5. **退出码语义化**：区分成功 / 任务失败 / 参数错误 / 预算中断。

### 3.2 结论

完全对齐 Claude Code 的 `-p` + `--output-format` 语义，是本仓库 harness 评测（`docs/agent-harness-assessment.md` 评分 15/100）的前置条件。最小可用集以 issue 建议为准，不引入 `--bare` / `--json-schema` 等超出范围的特性（后续 issue 再扩展）。

---

## 4. 推荐实现方案

### 4.1 最小可用集（对齐 issue 建议）

```bash
workx -p "<task>"                              # 非交互单次执行，text 输出
workx -p "<task>" --output-format json         # 最终消息 + 统计（turns/tool_calls/token/cost/耗时）
workx -p "<task>" --output-format stream-json  # NDJSON 实时流
workx -p "<task>" --permission-mode accept-edits # 无人值守档
workx -p "<task>" -p -                         # 从 stdin 读任务
```

### 4.2 关键设计决策

#### (a) 执行路径：复用 `ReActLoop` 还是 `ChatSession`？

| 路径 | 优点 | 缺点 |
|------|------|------|
| **直接构造 `ReActLoop` 同步 run（推荐）** | 绕过异步事件包装，直接拿到 `ReActResult`，无 EventBus 依赖，退出码自然映射 | 需手动做 `ChatSession` 做的装配（工具注册、系统提示词、压缩器、会话持久化） |
| 复用 `ChatSession::send_message` + 等待事件 | 复用完整装配 | 异步事件回传复杂，需等待 `StreamDoneEvent` 并提取结果，且 SessionStore/事件订阅耦合 TUI |

**推荐直接构造 `ReActLoop`**，装配逻辑复用 `factory.h` 的 `create_backend` / `register_builtin_tools` / `build_system_prompt`，形成一个新的轻量入口函数 `run_headless()`。

#### (b) 装配短路

在 `main.cpp` 参数解析阶段，若检测到 `-p/--print`，则：

1. **跳过**：首次运行向导、TUI `App`、文件索引、Island 灵动岛、models.dev 后台刷新。
2. **仍执行**：配置加载（`load_from_config_file` / `load_from_env`）、日志初始化、审计日志。
3. 直接进入 `run_headless()` → 同步执行 → 输出 → 按语义返回退出码。

#### (c) 退出码语义（对齐 issue）

| 退出码 | 含义 |
|--------|------|
| `0` | 成功 |
| `1` | 任务失败（ReAct 循环 error / goal 未达成） |
| `2` | 参数 / 配置错误 |
| `3` | 达预算上限中断 |

#### (d) 输出协议

- **结果写 stdout**；日志/进度写 stderr（复用现有 `liblogger`，其 file output 已独立，stderr 仅需确保 logger 不向 stdout 打印）。
- `--output-format json` 结构：

```json
{
  "result": "<final_answer 或 partial_content>",
  "session_id": "<uuid>",
  "usage": {
    "prompt_tokens": 0,
    "generated_tokens": 0,
    "total_tool_calls": 0,
    "total_iterations": 0,
    "duration_ms": 0
  },
  "goal_status": "Unknown",
  "was_error": false,
  "error_message": ""
}
```

- `--output-format stream-json`：**首版仅输出最终结果一行 + 每个 step（Thought/Action/Observation/FinalAnswer）各一行 NDJSON**；不做 token 级实时流（已确认，见 §8）。

#### (e) 权限模式

- 默认 `Default`（危险操作会尝试 `AskUser`，但 headless 无人应答 → 需处理）。
- `--permission-mode accept-edits`：映射到 `PermissionMode::AcceptEdits`，配合 `BypassPermissions` 支持无人值守。
- `--permission-mode strict`（#85）：映射到 `PermissionMode::Strict`。**无人值守场景推荐用它**——命令按白名单放行（`cmake --build` / `ctest` / `git status` / `python -m pytest` …），未命中白名单的命令走 `ask_user_confirm()`，而 headless 无确认通道 → fail-closed 拒绝；破坏性命令直接拒绝不询问。判定核心见 `src/agent/tool/command_policy.{h,cpp}`。
- `AskUserTool` 在 headless 下**自动拒绝**（返回结构化错误，不阻塞等待），依赖 `--permission-mode` 预授权（已确认，见 §8）。

### 4.3 涉及文件清单

| 文件 | 操作 |
|------|------|
| `src/tui/main.cpp` | 参数解析扩展 + headless 短路分支 |
| `src/agent/factory.h/.cpp` | 可选：新增 `run_headless()` 入口（复用 create_backend / register_builtin_tools / build_system_prompt） |
| `src/agent/headless/*.h/.cpp`（新增） | headless 执行器：同步构造 ReActLoop、跑任务、序列化结果 |
| `src/tui/CMakeLists.txt` | 注册新源文件（若复用 workx 可执行文件则无需新目标） |
| `docs/ENVIRONMENT_VARIABLES.md` | 记录新 CLI 参数 |

---

## 5. 备选方案对比

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| **A. 复用 ReActLoop 同步 run（推荐）** | 直接拿结果、无异步耦合、退出码清晰、与 harness 评测对齐 | 需手动装配 | ✅ 采用 |
| B. 复用 ChatSession 异步 + 等事件 | 复用完整装配 | 异步回传复杂、SessionStore/事件耦合 TUI、退出码映射困难 | ❌ 不采用 |
| C. 独立 `workx_headless` 可执行文件 | 隔离干净 | 双目标维护成本、与主程序装配易漂移 | ⚠️ 备选（若你偏好独立二进制） |
| D. 用 `--smoke` 扩展 | 已有 mock 流 | 只能跑 mock，无法跑真实 ReAct 链路，不满足验收 | ❌ 不采用 |

---

## 6. 潜在风险与边界情况

| 风险/边界 | 影响 | 应对 |
|-----------|------|------|
| **AskUserTool 死等** | headless 无人应答，权限确认卡死 | headless 下 AskUser 自动拒绝并返回结构化错误，或强制 `--permission-mode` 预授权 |
| **首次运行向导阻塞** | 无配置文件时进入交互向导 | headless 下跳过向导，配置缺失直接报错退出（码 2） |
| **模型无 tool_use 即停** | 任务未完成但正常返回 | 依赖后续 issue #78 的验证闭环；本 issue 仅保证链路可跑 |
| **stdout 污染** | 日志/进度混入 stdout 破坏 JSON | 严格分离：结果 stdout，日志 stderr + 文件 |
| **stdin 任务与交互冲突** | `-p -` 读 stdin 时与 TUI 输入冲突 | headless 分支根本不进 TUI，无冲突 |
| **预算无上限（关联 #79）** | 无人值守成本失控 | 复用现有 `max_iterations` + 后续 #79 的全局预算护栏；本 issue 先保证 `--permission-mode` 可用 |
| **FTXUI 链接开销** | headless 不需要 TUI 但仍链接 | 首版复用 workx 目标（含 TUI 链接），优化可后续拆目标（备选 C） |
| **跨平台无 TTY 检测** | Windows/CI 无 TTY 行为差异 | 用 `isatty` 探测，非 TTY + `-p` 强制 headless |

---

## 7. 验证与回滚方式

### 7.1 验证

1. **本地无 TTY**：`workx -p "列出当前目录" --output-format json | jq .result` 能解析。
2. **Docker / CI**：在 `ubuntu` 容器无 TTY 环境跑通一条真实 ReAct 链路。
3. **退出码**：分别验证 0/1/2/3 四种退出码。
4. **stdout/stderr 分离**：`2>/dev/null` 后 stdout 仍为合法 JSON。
5. **stdin**：`echo "任务" | workx -p -` 正常读入。
6. **回归**：不传 `-p` 时 TUI 行为不变（`workx` 直接进 TUI）。

### 7.2 回滚

- headless 为**新增分支**，不动 TUI 主路径（仅在 `main.cpp` 增加参数判断 + 短路）。
- 回滚：`git revert` 该分支提交，`-p` 参数消失即恢复纯 TUI 行为，无既有功能受损。

---

## 8. 决策记录（已确认）

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | 可执行文件形态 | ✅ **复用现有 `workx` 二进制**（`workx -p ...`），不建独立 `workx_headless` |
| 2 | `stream-json` 首版范围 | ✅ **最终结果 + 每步（step）NDJSON**，不做 token 级实时流 |
| 3 | AskUser 的 headless 行为 | ✅ **自动拒绝**（返回结构化错误），依赖 `--permission-mode` 预授权 |
| 4 | 成本统计 | ✅ **不含 `total_cost_usd`** |
| 5 | `--bare` 语义 | ✅ **首版不做**，后续扩展 |

---

_本方案已按上述决策定稿，未修改任何实现代码，等待实施批准。_
