# Issue #86 + #87 实现方案

> 状态：**已实现**（代码已落地并提交审查，PR #94）。本文保留为设计决策记录，不再是待审核方案。
> 基线：`develop` @ `b2d41ca`（已含 PR #92 子Agent护栏 / PR #93 git检查点）
> 分支：`fix/issue-86-87-silent-state-loss` —— 2 个提交：`9cdd167`（#86）/ `1fb4b27`（#87）

---

## 0. 决策摘要（已确认，逐条可审）

| # | 决策点 | 结论 | 关键依据 |
|---|---|---|---|
| D1 | 本轮范围 | 只做 `#86` + `#87`，其余 7 个 OPEN P1 排队 | 两者同为「状态/配置静默不生效」，改动集中在持久化层，可一次走通完整闭环并为后续建立范式 |
| D2 | #86 候选顺序 | `CLAUDE.md` > `AGENTS.md` > `AGENT.md`，**不加** `WORKX.md` | 保持现有 CLAUDE.md 优先语义不变，对本仓库零行为变更 |
| D3 | #87 持久化落点 | 新增 **append-only 的 `permission` 事件**，恢复时取最后一条 | 模式在会话中会多次变更，只有事件流能反映「最后一次处于什么模式」 |
| D4 | #87 连带字段 | 一并持久化 `sessionMode` / `beforePlanMode` / `inPlan` | ChatSession 里 Plan 是双轨（`m_permission_mode` + `m_session_mode`），只存前者 UI 会显示 Standard 但实际只读 |
| D5 | #87 fail-safe | 无事件 → 回退 `Default` + **显式告知**；事件值非法 → 回退 `Plan` | 历史会话里 Plan 模式是极少数，强行切 Plan 会误伤大多数旧会话 |
| D6 | 验收策略 | 完整单测 + 按 issue 验收标准手工验证 | #86 当前零单测；#87 的 fail-safe 三分支需分别守 |
| D7 | 工作区处理 | #81/#79 合并后 WIP 已清空，直接基于 `develop` 建分支 | 原先 `feat/issue-81-git-checkpoint` 的 7 文件 +150 行 WIP 已随 PR #93 落地，工作区只剩未跟踪的 docs 文件 |
| D8 | #87 的 UI 提示方式（原待确认项 P-1） | **复用 transcript 插一行 + LOG_WARN**，不新增事件类型 | `App::resume_session` 末尾（`tui/app.cpp:927-929`）已有 `m_vm.apply(ActionAppendMessage{...})` 的现成范式；文案集中到 `theme/strings.h`（#81 刚建立该范式） |

---

## 1. 问题背景与目标

### 1.1 `#86` 项目记忆只认 CLAUDE.md / AGENT.md：AGENTS.md 静默不生效

**现象**：`src/agent/prompt/memory.h` 的 `load_project_memory()`（`:45`）逐级向上遍历目录，每级只尝试 `CLAUDE.md`（`:68`）和 `AGENT.md`（`:80`），**`AGENTS.md` 从未被尝试**。

**为什么危险**：本仓库根目录**同时存在** `AGENTS.md`（2510B）和 `CLAUDE.md`（2520B），且内容有差异——因为 `CLAUDE.md` 被优先命中，问题一直被掩盖。一旦某个项目只有 `AGENTS.md`（比 `AGENT.md` 更常见的社区约定），项目规范**完全不生效且无任何提示**。这是最难排查的静默失败类型。

**目标**：把候选文件扩展为 `CLAUDE.md` > `AGENTS.md` > `AGENT.md`，每级取第一个存在的（保持现有「每级一个」语义）。

### 1.2 `#87` PermissionMode 不随 resume 持久化：Plan 只读边界静默丢失

**现象**：用户在 Plan 模式下批准方案、会话中断后 resume，agent 变成普通模式——**可以写入和执行**，而用户以为还在只读阶段。属于安全边界静默降级。

**代码已承认此问题**（`src/agent/core/react_loop.h:301-306`）：

```
/// @note 评审 #2：该模式为 ReActLoop 内存成员，不随会话压缩/恢复或
///       /resume 切换持久化，跨这些操作会重置为 Default（Plan 只读边界
///       静默丢失）。
```

**⚠️ 与原 issue 描述的重要偏差**：issue 建议改 `ChatSession::load_session`（`chat_session.cpp:1726`）。但勘察确认 **`save_session` / `load_session` 在 `src/` 下无任何生产调用点**，只被单测引用（`tests/unit/agent/core/test_chat_session.cpp:88,101,108`），是一条死路径。

真实 resume 链路是：

```
/resume 斜杠命令          tui/command/builtins.cpp:105-113
  → App::cmd_resume       tui/app.cpp:825-859
  → App::resume_session   tui/app.cpp:861-930
  → ChatSession::switch_session   chat_session.cpp:455-509
       ├─ SessionStore::load_messages    :462
       ├─ SessionStore::load_meta        :465
       ├─ new SessionStore(...) + open   :483-487
       ├─ TodoStore::restore_todos       :498-499
       ├─ persist_system_prompt("resume") :501-504
       └─ #81 GitCheckpoint::reset()      :496
```

`switch_session` 恢复 session_id / messages / session_store / compactor 基线 / todo / system_prompt，**恰好不恢复** `m_permission_mode`、`m_permission_mode_before_plan`、`m_session_mode`。

**目标**：改 `SessionStore` 事件流 + `switch_session`，而非 `load_session`。

### 1.3 为什么这两个放一起

同属「状态/配置**静默**不生效」：一个是「写了规范文件但不生效」，一个是「设了只读边界但 resume 后失效」。两者都满足：故障无提示、用户无从察觉、修复面小而集中、验收标准明确。合一个分支可在一次迭代内走通「读 issue → 改 → 单测 → 按验收标准手工验证」的完整范式。

---

## 2. 影响范围

### 2.1 `#86`

- **唯一生产调用点**：`src/agent/factory.cpp:505-510`（`load_and_format_project_memory`）——影响面极小。
- **受影响行为**：仅「项目级记忆文件的选取」。对同时有 `CLAUDE.md` 的项目**零行为变更**；对只有 `AGENTS.md` 的项目，从「无规范」变为「加载规范」（即修复本身）。
- **不受影响**：`format_project_memory()` 的输出格式、`system_prompt` 事件、`docs/full-guide.md`（勘察确认该文件**未**描述记忆文件名，无需同步修改）。

### 2.2 `#87`

- **状态源**：`ChatSession::m_permission_mode`（`chat_session.h:509`）、`m_permission_mode_before_plan`（`:511`）、`m_session_mode`（`:514`）。
- **模式变更出口（全部 5 处，均为持久化写入点）**：

  | 出口 | 位置（`b2d41ca`） | 触发 |
  |---|---|---|
  | `set_permission_mode` | `chat_session.cpp:997` | CLI `--permission-mode` / `factory.cpp:227-230` |
  | `toggle_permission_mode` | `chat_session.cpp:1011` | Shift+Tab 两态切换 |
  | `set_session_mode` | `chat_session.cpp:1038` | Plan 进出联动 |
  | `toggle_session_mode` | `chat_session.cpp:1058` | 三态循环 |
  | 权限状态回写 lambda | `chat_session.cpp:~1266`（`query_engine.set_session_mode` 在 `:1274`） | `EnterPlanMode` / `ExitPlanModeV2` 工具经 `react_loop.cpp:924-944` 回调 |

- **消费方（只读，不改）**：`ToolContext::permission_mode`（`context.h:144`）及 11 个工具的 `check_permissions`、`QueryEngine::apply_permission`（`query_engine.cpp:17-30`）、`ReActLoop::apply_permission_state`（`react_loop.h:318-323`）。

### 2.3 明确不在本轮范围内

| 项 | 说明 |
|---|---|
| `#52 #82 #83 #84 #85 #88 #89` | 其余 7 个 OPEN P1，排队 |
| **headless 路径** | `headless.cpp:212,232` 直接给 ReActLoop 设模式、不经 ChatSession；headless 是单次执行不做 resume，**不写入** permission 事件 |
| **子 Agent** | `agent_tool.cpp:380,405` 继承父模式，生命周期限于单次任务，**不写入** |
| **PlanCoordinator 状态恢复** | `plan_coordinator.h:99-100` 的 serialize/deserialize 在 `src/` 下**无生产调用点**。本轮只恢复*模式*，不恢复*规划状态*——见 §6 R3 |
| **`save_session`/`load_session`** | 死路径，不动（避免为无调用方的代码增加负担） |
| `#89 的错误码透传前置修复` | `react_loop.cpp:696` 吞掉 HTTP 状态码的独立缺陷，属于 #89 范畴 |

---

## 3. 技术思路与关键步骤

### 3.1 `#86`（预计 ~10 行代码 + ~60 行单测）

**步骤 1**：`src/agent/prompt/memory.h:67-88`，把「二选一」改为「三选一，按顺序取第一个存在的」。

```cpp
// 候选顺序：CLAUDE.md > AGENTS.md > AGENT.md（每级只取第一个存在的）
static constexpr std::string_view kMemoryFileCandidates[] = {
    "CLAUDE.md",
    "AGENTS.md",
    "AGENT.md",
};
for (std::string_view name : kMemoryFileCandidates) {
    const std::filesystem::path candidate = dir / name;
    if (std::filesystem::exists(candidate, ec) && !ec) {
        std::ifstream ifs(candidate, std::ios::binary);
        if (ifs) { /* 读取 → push_back → break */ }
    }
}
```

**步骤 2**：同步更新 `memory.h` 的文件头注释（`:4-6`、`:10-12`、`:40-44`）与 `factory.cpp:503` 的调用点注释。注释错误本身就是这次静默失败的一部分成因。

**步骤 3**：新增 `tests/unit/agent/prompt/test_memory.cpp`。`tests/unit/CMakeLists.txt:38-39` 用 `GLOB_RECURSE` 自动收集，**无需改 CMake**。

### 3.2 `#87`（预计 ~90 行代码 + ~140 行单测）

**步骤 1**：`session_store.h` 增加事件载荷结构 + `append_permission()` / `load_permission()` 声明。

事件形态**复用 `title` 事件的既有模式**（`session_store.cpp:215-222`，append-only、读取时取最后一条），载荷：

```json
{
  "type": "permission",
  "sessionId": "...",
  "timestamp": "...",
  "permissionMode": "plan",
  "sessionMode": "plan",
  "permissionModeBeforePlan": "default",
  "inPlan": true
}
```

> 枚举**序列化为字符串**而非整数：与 `system_prompt` 事件的 `reason` 字段（`initial`/`changed`/`resume`）风格一致，便于排查；反序列化失败即视为「非法值」走 D5 的从严分支。

**步骤 2**：`session_store.cpp` 实现 append 与 load。

- `append_permission()` 放在 `append_title`（`:215`）之后，保持事件类型登记顺序一致。
- `load_permission()` 声明为 **static、入参 `file_path`**——与 `load_todos`（`:407`）/`load_meta`（`:339`）/`load_sub_agents`（`:422`）同形态，`App::resume_session` 已在用这种静态调用。取最后一条的实现直接照抄 `load_todos` 的写法（`:412` 命中即 `todos.clear()` 再重建）。

**步骤 3**：在 §2.2 列出的 **5 个模式变更出口**统一写入事件。收敛方式建议：在 `ChatSession` 内加一个私有 `persist_permission_state()`，5 个出口各自调用——**不做**隐式拦截（如监听 `notify_permission_state`），因为该回调在 ReActLoop 每轮都可能触发，会造成事件膨胀。

**步骤 4**：`ChatSession::switch_session`（`chat_session.cpp:454-509`）末尾增加恢复逻辑，位置放在 `persist_system_prompt("resume")`（`:500-503`）之后：

```
load_permission()
  ├─ 命中且值合法 → 恢复 mode / before_plan / session_mode
  ├─ 无事件      → 保持 Default，标记 restored=false, reason="no_record"
  └─ 值非法      → 回退 Plan（从严），标记 restored=false, reason="invalid_value"
```

**步骤 5**：显式告知（D5 的强制项；实现方式按 **D8**，复用 transcript + LOG_WARN，不新增事件类型）。

分两层：

1. **agent 层（必做）**：`switch_session` 内两条 fail-safe 分支各打一条 `LOG_WARN`，含 session_id 与 reason。

2. **UI 层（必做）**：复用 `App::resume_session` 末尾的既有范式——

```cpp
// tui/app.cpp:927-929（现有代码，范式来源）
m_vm.apply(ActionAppendMessage{
    .role = "assistant",
    .text = std::string(str::kResumedPrefix) + title + std::string(str::kMdBoldEnd)});
```

在其后追加一条**条件**提示。为此 `ChatSession` 暴露上次恢复结果：

```cpp
// chat_session.h
struct PermissionRestoreResult {
    bool restored = false;      // 是否从会话记录恢复成功
    std::string reason;         // "no_record" / "invalid_value" / ""（成功时为空）
    tool::PermissionMode mode = tool::PermissionMode::Default;
};
PermissionRestoreResult last_permission_restore() const;   // 受 m_state_mutex 保护
```

`switch_session` 填充 `m_last_permission_restore`；`App::resume_session` 读取后**仅当 `!restored`** 时插一行。文案新增到 `theme/strings.h`（紧邻 `kResumedPrefix`，`:221`）：

- `kPermRestoreNoRecord` — 会话未记录权限模式，已回退为 default
- `kPermRestoreInvalid` — 权限模式记录非法，已从严回退为 plan

> **为什么不让 App 直接调 `SessionStore::load_permission(file_path)`**：fail-safe 的三分支判定（无事件 / 值非法 / 成功）是 agent 层语义，TUI 再判一遍会造成两处逻辑漂移。由 ChatSession 单点产出结果，TUI 只负责呈现。

**步骤 6**：单测。

- `tests/unit/agent/session/test_session_store.cpp`：permission 事件 append / 读取往返、多条取最后一条、坏值跳过。
- `tests/unit/agent/core/test_chat_session.cpp`：`switch_session` 三用例（正常恢复 / 无事件回退 Default / 非法值回退 Plan）。可参照现有 `restore_from_file` 用例（`:411-447`）的构造方式。
- 补充：**当前无任何测试覆盖「resume 后 permission mode 是否保留」**，这是本次要补上的核心缺口。

---

## 4. 涉及的文件和模块

| 文件 | 改动类型 | 锚点（基线 `b2d41ca`） | 预估行数 | issue |
|---|---|---|---|---|
| `src/agent/prompt/memory.h` | 改（候选列表 + 头注释 `:4-12`、`:40-43`、`:67-80`） | `:45` `load_project_memory` | +8 / -6 | #86 |
| `tests/unit/agent/prompt/test_memory.cpp` | **新增** | GLOB 自动收集，无需改 CMake | ~60 | #86 |
| `src/agent/session/session_store.h` | 增（载荷结构 + `append_permission` / static `load_permission`） | 参照 `:142` `append_title`、`:185` `load_todos` | ~20 | #87 |
| `src/agent/session/session_store.cpp` | 增（append / load） | `append_title` `:215`、`load_todos` `:407` | ~35 | #87 |
| `src/agent/core/chat_session.h` | 增（`PermissionRestoreResult` + persist 方法 + getter） | `:509` / `:511` / `:514` 状态成员 | ~12 | #87 |
| `src/agent/core/chat_session.cpp` | 改（5 个出口写入 + `switch_session` 恢复） | `switch_session` `:455`（`GitCheckpoint::reset()` 在 `:496`，新增紧随其后） | ~50 | #87 |
| `src/tui/app.cpp` | 改（resume 后条件提示） | `resume_session` `:861`，插在 `:927-929` 之后 | ~10 | #87 |
| `src/tui/theme/strings.h` | 增（2 条提示文案） | 紧邻 `kResumedPrefix` `:221` | ~4 | #87 |
| `tests/unit/agent/session/test_session_store.cpp` | 增（事件往返用例） | — | ~50 | #87 |
| `tests/unit/agent/core/test_chat_session.cpp` | 增（`switch_session` 三用例） | 参照 `restore_from_file` 用例 | ~90 | #87 |
| `docs/full-guide.md` | **不改**（勘察确认未描述记忆文件名） | — | 0 | — |

**合计**：约 129 行生产代码 + 约 200 行测试。

> **行号已按 `b2d41ca` 校准**。PR #92 给 `context.h` 加 75 行（`PermissionMode` 现 `context.h:68`、`SessionMode` `:81`、`ToolContext::permission_mode` `:214`、`session_mode` `:219`），但**未改变语义**；PR #93 只给 `chat_session.cpp` 加了 include 与 `switch_session` 的步骤 5。
>
> ⚠️ **注意 `src/agent/CMakeLists.txt` 是显式源文件列表（非 GLOB）**——本次不新增 `src/` 下的 .cpp，故无需改动；但 `tests/` 下新增的 `test_memory.cpp` 走 `GLOB_RECURSE`，会自动收集。

---

## 5. 验收标准

### 5.1 自动化（D6）

- [ ] `test_memory.cpp`：`CLAUDE.md` / `AGENTS.md` / `AGENT.md` 各自单独存在时均能命中
- [ ] `test_memory.cpp`：三者同时存在时取 `CLAUDE.md`；`AGENTS.md` + `AGENT.md` 同时存在时取 `AGENTS.md`
- [ ] `test_memory.cpp`：无任何候选文件时不报错、返回空
- [ ] `test_session_store.cpp`：permission 事件 append → load 往返一致
- [ ] `test_session_store.cpp`：多条 permission 事件取最后一条
- [ ] `test_chat_session.cpp`：`switch_session` 后 `permission_mode()` 等于持久化值
- [ ] `test_chat_session.cpp`：无 permission 事件 → 回退 `Default`
- [ ] `test_chat_session.cpp`：非法 permission 值 → 回退 `Plan`
- [ ] 现有 400+ 单测全绿（尤其 `test_permission_security.cpp`、`test_plan_mode_tools.cpp`、`test_react_loop.cpp` 的权限相关用例）

### 5.2 手工（对齐两篇 issue 原文）

- [ ] **#86**：建一个只含 `AGENTS.md` 的临时目录，在其中启动的对话能引用到 `AGENTS.md` 里的约定
- [ ] **#87**：Plan 模式下开会话 → 退出 → `/resume` → 尝试 `FileWrite`：仍被拒绝，且 UI/日志显示当前处于 Plan 模式
- [ ] **#87 反向**：Default 模式会话 resume 后不出现「莫名变只读」的误伤

---

## 6. 潜在风险与备选方案

| ID | 风险 | 概率/影响 | 缓解 | 备选方案（未采纳及原因） |
|---|---|---|---|---|
| **R1** | 模式变更出口遗漏，某条路径没写事件，resume 后仍静默丢失 | 中 / 高 | 5 个出口全量清点（§2.2）+ 每个出口一个恢复单测 | 隐式拦截 `notify_permission_state` 统一写入——**不采纳**：该回调 ReActLoop 每轮都可能触发，事件会膨胀 |
| **R2** | `SessionStore` 并发写安全 | 低 / 低（本改动） | 已确认：`append_line`（`session_store.cpp:136-141`）无锁，靠 `ofstream` + 每条 flush。本方案新增的 permission 写入**全部在主线程**（5 个模式变更出口），不引入新的并发。**但存在既有隐患**：`append_sub_agent`（`chat_session.cpp:1816,1831`）由子 Agent 事件订阅回调触发，而子 Agent 事件从线程池发布（`agent_tool.cpp:143-155,187-193`），理论上可从工作线程写 `m_out`。这是**既有问题、不在本轮修复**，仅登记 | 无 |
| **R3** | 恢复 Plan **模式**但 `PlanCoordinator` 状态为空（其 serialize/deserialize 无生产调用点） | 高 / 中 | 本轮显式声明**不恢复规划状态**（§2.3），在 fail-safe 提示中一并告知用户「模式已恢复，规划上下文需重新生成」 | 一并恢复 PlanCoordinator——**不采纳**：需先补它的生产调用点，属独立改动，量级超出本轮 |
| **R4** | #86 使部分项目的 system prompt 变长（只有 `AGENTS.md` 的项目新增一段） | 中 / 低 | `factory.cpp` 侧**无 system prompt 截断层**（已确认）；记录为已知限制 | 在 `memory.h` 内加大小上限截断——**不采纳**：会静默丢规范内容，与本次「消除静默失败」的目标相悖 |
| **R5** | 将来要支持 `WORKX.md` 需再改一次候选列表 | 低 / 低 | D2 已明确接受；届时只是数组加一项 | 本轮一并加 `WORKX.md`——用户已选不加 |
| **R6** | `permission` 事件取值非法（文件被手工编辑 / 跨版本枚举扩展） | 低 / 高 | 反序列化失败即从严回退 `Plan` + 显式告知（D5） | 无 |
| **R7** | #86 若把优先级改成 `AGENTS.md` 优先，本仓库加载内容会变（两份文件有差异） | — | **已规避**：D2 保持 `CLAUDE.md` 优先，本仓库行为不变 | `AGENTS.md` 优先（评估文档 `docs/agent-harness-assessment.md:108` 的顺序）——**不采纳**：会悄然换掉现有会话遵循的规范 |

**新增风险（基线推进后补充）**：

| ID | 风险 | 概率/影响 | 缓解 |
|---|---|---|---|
| **R8** | `switch_session` 的步骤编号冲突：#93 已占用「步骤 5 = `GitCheckpoint::reset()`」（`chat_session.cpp:496`） | 高 / 低 | 权限恢复登记为**步骤 6**，紧随其后；两者无依赖关系，顺序无关，但注释编号必须连续，否则后人误读 |

**待确认项**：无（P-1 已由 D8 解决）。

---

## 7. 交付切分与分支

两个独立 commit，便于单独 revert：

1. `fix(#86): 项目记忆候选文件名增加 AGENTS.md` — 含 `memory.h` 改动 + `test_memory.cpp`
2. `fix(#87): PermissionMode/SessionMode 随会话持久化并在 resume 恢复` — 含 session_store / chat_session / tui 改动 + 两组单测

**分支状态**：✅ **已创建并已切换** —— `fix/issue-86-87-silent-state-loss`，基于 `develop @ b2d41ca`。

```
git checkout develop && git pull --ff-only     # 1e52ae1 → b2d41ca（PR #92/#93）
git checkout -b fix/issue-86-87-silent-state-loss
```

原先阻塞建分支的问题已自然消解：#81 的 7 文件 +150 行 WIP 已随 PR #93 落地，工作区只剩未跟踪的 docs 文件（`.workbuddy/`、`docs/agent-harness-*`、`docs/p0-test-plan.md`、本方案文档），不影响切换。

**基线推进带来的两点影响（已回写本文档）**：

1. `context.h` 因 #92 增加 75 行（新增 `SubAgentBudget`），行号已重新校准，语义不变。
2. `switch_session` 因 #93 新增步骤 5（`GitCheckpoint::reset()`），权限恢复顺延为步骤 6 —— 见 R8。

**开发期间无需 rebase**：#81 已落地，本分支即基于其合并后的 develop。

---

## 8. 后续排队（本轮不做，供参考）

按 B1 勘察的依赖与量级排序：

| 组 | Issue | 备注 |
|---|---|---|
| 可靠性 | `#89` | **前置缺陷已修复**（PR #95）：`http_client` → `SSEStreamReader` → `IStreamReader` 错误通道 → `ThoughtResult`/`ReActResult.http_status` → `compute_retry` 已端到端打通，4xx 不再被误判为可重试。降级主体（`fallback_models` 配置 + backend 切换 + 降级审计）待做。另依赖仍是 OPEN 的 `#55`（预算，P2） |
| 上下文/编排 | `#82` `#83` | 同属 prompt 层注入，改 `factory.cpp:452-491`（环境段）与 `react_loop.cpp:1059`（tool_result 回注） |
| 安全 | `#84` `#85` `#88` | `#84` Windows 沙箱涉及平台原生代码（Job Object / AppContainer / 仅暴露 degraded 三档成本相差一个数量级）；`#88` 需评估是否引入系统凭据存储（当前 vcpkg 无任何相关依赖） |
| 子 Agent | `#52` | **描述已大面积过时**：`TaskStopTool`/`TaskOutputTool` 已注册、`run_in_background` 默认已为 true。真正缺的只剩 fork 复用父 prompt（`agent_tool.cpp:131` 每次重建，父 system prompt 根本不下发）、trace_id/depth（`AuditEvent::trace_id` 字段存在但**全仓零写入点**）、SendMessage 队列 |
