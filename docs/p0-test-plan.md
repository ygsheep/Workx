# P0 功能测试用例方案

> 适用范围：Issue #77 ~ #81（Agent Harness 评分卡中的 5 项 P0）
> 编写日期：2026-09-30 · **复审更新 12:30** · **二次更正 13:20** · **三次更新 18:55**（develop `9a2de19`）
> 状态：方案待评审；#77 / #79 / #80 / #81 均已合入 develop；**#78 的 P1 基建 + P2 headless 已落地**（PR #98），P3 / P4 待做
>
> 🔴 **二次更正说明**：初版与复审均把 #78 判为「代码未实现 / 零实现」，**该判定不成立**。详见 §1.2 E。
> ✅ **三次更新（2026-09-30 18:39，`9a2de19`）**：#78 已落地 P1 / P2 —— `ReActLoop` 自带 FinalAnswer 前验证门禁
> （`run_verification_gate()`），headless 入口默认开启；13 条 `[issue78]` 用例全绿。VF-08 / P3 / P4 仍未做。

---

## 0. 前提假设与事实校正

### 0.1 实现状态（复审已更新到 commit 级）

> 📌 本方案初版编写时 #79 / #81 尚未合入。**现均已合并到 develop**，下表为复审后的实际情况。
> 📌 **二次更正**：#78 行的「现有测试：无 / ❌ 需先实现」是错的。`#31/#32` 的目标验证栈已存在**且有测试**（`test_agent_core.cpp:95-111`），
> 缺的是**接入默认 ReAct 路径 + 默认开启**。本 issue 的性质是「接线 + 改默认值」，不是「从零建验证器」。

| Issue | 功能 | GitHub 状态 | develop 合并提交 | 现有测试 | 可开工 |
|---|---|---|---|---|---|
| #77 | headless 非交互模式 | CLOSED | `d34d44a`（PR #90） | ✅ **已补齐**：`test_headless.cpp` 13 条，HL-01~HL-12（PR #100 `241f014`） | ✅ **已完成** |
| #80 | json_schema 通用参数校验 | CLOSED | `d34d44a`（PR #90） | ✅ 8 用例 + `test_json_schema_wiring.cpp` 覆盖 JS-15/JS-16（PR #101 `edd2608`） | ✅ **已完成** |
| #79 | 子 Agent 派生护栏 | CLOSED | `67ea633`（PR #92） | 5 用例，`test_sub_agent_budget.cpp` | ✅ |
| #81 | git 检查点与回滚 | CLOSED | `b2d41ca`（PR #93） | 7 用例，`test_git_checkpoint.cpp` | ✅ |
| #78 | 强制验证闭环 / PreCompletion 门禁 | **OPEN**（部分落地） | ✅ `9a2de19`（PR #98，P1+P2） | ✅ **13 条 `[issue78]` 用例全绿**（`test_react_loop.cpp` 8 条 + `test_agent_core.cpp` 5 条） | ⚠️ P1 / P2 已完成；P3 / P4 与 VF-08 待做 |

**结论（五次更新后，2026-10-01）**：
- **阶段 0 / 1 / 2 已完成** —— `mock_git_repo.h`（PR #100）、`headless_internal.h` 注入口（PR #100）、HL-01~HL-12 共 13 条（PR #100）、JS-15/JS-16（PR #101）**全部已落地**；
- **阶段 6 已落地** —— 新增 `.github/workflows/build-test.yml`，并在 WSL 上按真实 CI 步骤完成预演；首轮即暴露 5 类 Linux 平台缺陷（#104 / #105 / #107 / #109），门禁先以 `continue-on-error` 观察。见 §3.2；
- #78 的 P1 / P2 已于 2026-09-30 落地（PR #98）——门禁进主循环 + headless 默认开启，VF-01 ~ VF-07 / VF-09 共 13 条用例全绿。**剩余缺口**：P3（交互式默认开启）、P4（命令自适应探测）、VF-08；
- **当前真正待办**：清 #104 / #105 / #107 / #109（CI 转硬门禁的前置）→ 阶段 3（#79 补边界）→ 阶段 4（#81 补真仓库用例）→ 阶段 5（#78 P3/P4/VF-08）。

**评分同步（五次更新）**：Harness 加权总分 **61.05 → 68.5 →（记账更正）69.9 → 71.9**。**CI 接入不改变评分**——评测维度衡量的是「有没有接入基准并跑出分数」，跑通单测属于工程化基建，不等于 harness 能力提升。详见 `docs/agent-harness-scorecard.html`。

**此前记录的风险，现均已闭环**：
1. ✅ ~~headless 零测试~~ → PR #100 补齐 13 条 HL-01~HL-12；
2. ✅ ~~headless 无法注入 mock 后端~~ → PR #100 新增 `headless_internal.h`，暴露 `run_headless_with_provider()` 及原先锁在匿名 namespace 的 8 个内部函数；
3. ✅ ~~#80 只测了纯函数~~ → **更正**：JS-15 早已由 `test_json_schema.cpp:160` 的 `[json_schema][executor]` 覆盖（此前漏看第 8 条）；JS-16 由 PR #101 的 `test_json_schema_wiring.cpp` 补齐。

**当前剩余风险**：
- ✅ ~~headless 与 JS-16 用例尚未进 CI~~ → **阶段 6 已落地**（`.github/workflows/build-test.yml`）；
- 🔴 **Linux 上 23 / 1342 用例失败，CI 暂为软门禁** —— 5 类平台缺陷，已开 #104 / #105 / #107 / #109；
  其中 #105（island IPC `close()` 缺 `shutdown()`）会让 `ctest` **挂死**，是最关键的一项；
- 🔴 **更正：Windows 上 ctest 并非「静默跳过」，而是「必然失败」** —— 实测 **196 / 1319 失败**，
  且与该批次**名字含中文的用例数 196 精确吻合**（#106）。PR #98 的 `chcp 65001` 只修了
  「测试发现阶段」，未覆盖「执行阶段的 argv 传递」。**含义：Windows 上 ctest 不可用；
  平时直接跑 exe（全量、不做名称过滤）所以长期未被发现。**

### 0.2 技术栈与基建假设（已核实，非假设）

| 项 | 实际情况 |
|---|---|
| 测试框架 | Catch2 v3（`find_package(Catch2 CONFIG REQUIRED)`），自定义 main 在 `tests/test_main.cpp` |
| 构建 | CMake + vcpkg；`WORKX_BUILD_TESTS` 默认 **ON**，`WORKX_BUILD_INTEGRATION_TESTS` 默认 **OFF** |
| 测试目标 | 4 个独立可执行文件：`core_` / `agent_` / `island_` / `tui_` + `_unit_tests` |
| 源文件收集 | `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` → **新增 `.cpp` 自动纳入，无需改 CMake** |
| Mock 基建 | `tests/unit/helpers/`：`mock_config_manager.h` / `mock_event_bus.h` / `mock_provider.h` / `mock_task_manager.h` |
| 脚本化模型 | `MockCompletionProvider` + `MockStreamReader` 支持 `add_content_chunk / add_tool_use_start / add_tool_use_delta / error_at / cancel_after`，`deque` 串多轮 → **可无真模型驱动完整 ReActLoop**（`tests/unit/agent/core/test_react_loop.cpp` 已是范例） |
| 标签体系 | `catch_discover_tests(ADD_TAGS_AS_LABELS ON)` → Catch2 tag 自动映射为 ctest label；`[slow]` 已被 `ctest -LE slow` 使用 |
| CI | ✅ 已新增 `.github/workflows/build-test.yml`（build + `ctest -LE slow`，ubuntu）；原 `code-quality.yml` 只有 complexity / format / clang-tidy / cppcheck 四个 job，`release.yml` 有 build 但**不跑测试** |

### 0.3 需要新增的基建（否则部分用例写不了）

1. **`mock_git_repo.h`**：RAII 临时 git 仓库 fixture（init / commit / 改 / 删 / 未跟踪 / rename）。#81 的真仓库用例需要。
2. **headless 后端注入口**：`run_headless()` 内部直接调 `create_backend(cfg, ...)` 建真实后端，**测试无法注入 `MockCompletionProvider`**。需抽出内部重载（如 `run_headless_with_provider(cfg, task_manager, provider, opts)`）。
   → ⚠️ **缺口不止主流程**：`parse_permission_mode` / `result_text` / `step_json` 在**匿名 namespace**，`resolve_backend` / `build_loop` / `render_output` / `derive_exit_code` / `execute_task` **未在 `headless.h` 声明** → 测试**连纯函数也拿不到**（初判「只能测输出序列化纯函数」低估了缺口）。需一并把可测件以 `@internal` 区块导出。
   → 这是 #77 用例能否落地的**阻塞项**，建议与用例同 PR 提交。
3. **`scripted_model.h`（可选）**：把「第 N 轮返回指定 tool_call / 文本」封装成一行 DSL，供多轮脚本化用例复用。✅ #78 落地时**未新增**该基建，而是以 `VerificationGateFixture` 等价实现（见 §1.2 E）。

---

## 1. 用例选择

### 1.1 筛选标准

四条准入规则，任一命中即入选；按 P1→P4 排序，P1 未覆盖不得声称功能已交付。

| 级别 | 类别 | 判据 | 准入条件 |
|---|---|---|---|
| **P1** | 核心主流程 | Issue 验收标准里写「必须」的那几条 | 每条验收标准 ≥ 1 个用例 |
| **P2** | 异常分支 | 代码里每个 `return` 错误码 / 每个回退分支 | 每个 exit_code、每个 `nullopt` 分支 ≥ 1 个用例 |
| **P3** | 边界条件 | 阈值 ±1、空输入、非目标环境（非 git 仓库）、超长/嵌套 | 每个可配置阈值的上下边界各 1 个 |
| **P4** | 回归场景 | 跨模块耦合 + 「曾修过又坏」 | 模块交叉点 ≥ 1 个 |

风险排序辅助：`优先级 = 影响面 × 触发概率 × 漏测后检测难度`。
按此，**#77 headless 排第一**——它是评测链路入口、零覆盖、且失败模式是「静默输出错内容」而非崩溃（最难发现）。

### 1.2 用例清单

#### A. #77 headless（12 条，最高优先）

| ID | 类别 | 用例 | 断言要点 |
|---|---|---|---|
| HL-01 | P1 | text 格式输出 | `output == final_answer + "\n"` |
| HL-02 | P1 | json 格式输出 | 可 `json::parse`；含 `result / session_id / usage / goal_status / was_error` 六个字段 |
| HL-03 | P1 | stream-json 输出 | 多行 NDJSON，**每行独立可 parse**；末行为 `result_json` |
| HL-04 | P2 | 后端创建失败 | `exit_code == 2`，output 含「无法创建后端」 |
| HL-05 | P2 | 未知 permission_mode | `exit_code == 2`，output 含未知模式名 |
| HL-06 | P2 | 任务失败 `was_error` | `exit_code == 1` |
| HL-07 | P2 | 被中断 `was_interrupted` | `exit_code == 1` |
| HL-08 | P3 | final_answer 空 → 回退链 | 依次回退 `partial_content` → `error_message`（`headless.cpp:44-49`） |
| HL-09 | P3 | 四种 permission_mode 映射 | `default / accept-edits / bypass-permissions / plan` 均解析成功 |
| HL-10 | P3 | task 为空串 | 不崩溃，走正常流程 |
| HL-11 | P1 | **无人值守不阻塞** | AskUser 工具被调用时不挂起（`event_bus=nullptr` 路径，`headless.cpp:141-142`）——需超时断言兜底 |
| HL-12 | P4 | headless × #81 耦合 | git 仓库且有改动 → 输出含 `git_diff_summary` / 改动清单；非 git 仓库 → 字段缺失且无异常 |

> ⚠️ **顺带发现的疑似缺陷**：`parse_permission_mode` 接受 `"plan"`（`headless.cpp:40`），但错误提示串只列了 `default / accept-edits / bypass-permissions`（`headless.cpp:215`）。HL-09 会暴露这个不一致，建议在同 PR 决定「补进提示串」还是「headless 下显式拒绝 plan」。

#### B. #80 json_schema（现有 8 条，补 8 条）

| ID | 类别 | 用例 | 断言要点 |
|---|---|---|---|
| JS-09 | P3 | integer vs number | `1.0` 对 `integer` 应为错 |
| JS-10 | P3 | 空 schema / 非 object schema | 宽松放行，`ok == true` |
| JS-11 | P3 | `additionalProperties: false` | 仅告警不拒绝，`ok` 仍为 true（对齐弱模型兼容设计） |
| JS-12 | P3 | 深层嵌套 object | 递归受限，不栈溢出 |
| JS-13 | P2 | 多错误聚合 | `errors.size() > 1`；`to_string()` 全部包含 |
| JS-14 | P2 | `is_missing` 标记 | 缺必填 → `is_missing == true`（供 MissingArgument 映射） |
| JS-15 | **P1** | **执行侧真的调用了校验** | 通过 `ToolRegistry` 执行一次带错参的工具调用 → 返回 schema 错误，**不落到工具实现**。✅ **已覆盖**：`test_json_schema.cpp:160` 的 `[json_schema][executor]`（与另 7 条**同属 PR #90 `d34d44a`**）即此用例——真调 `ToolExecutor::execute()`，验 `MissingArgument` / `InvalidInput` / ok 三分支。~~「当前 8 条只测了纯函数，等于 #80 只做了一半」~~ 系把第 8 条漏看所致 |
| JS-16 | **P1** | **错误回灌后自纠** | 脚本化两轮：第 1 轮坏参数 → 第 2 轮好参数 → 工具最终执行成功 |

#### C. #81 git checkpoint（现有 7 条，补 8 条）

| ID | 类别 | 用例 | 断言要点 |
|---|---|---|---|
| GC-08 | P2 | 非 git 仓库 | `valid == false`，`diff_since_base()` 空，**不抛异常、不阻断** |
| GC-09 | P3 | 捕获时工作区不干净 | `clean_at_capture == false` |
| GC-10 | P3 | 切换 cwd 到另一仓库 | 丢弃旧基线重新捕获（防串仓，`git_checkpoint.h:59-60`） |
| GC-11 | P1 | 未跟踪新文件 | `status == "?"` |
| GC-12 | P3 | rename 路径归一化 | `dir/{a => b}.cpp` → `dir/b.cpp` |
| GC-13 | P1 | `rollback_tracked` 只回滚 M/D | 新增与未跟踪进 `skipped`，**文件不被删除** |
| GC-14 | P4 | 并发安全 | `capture / reset / diff` 跨线程并发无数据竞争（mutex 已加，需 TSAN 或 `[slow]` 压测） |
| GC-15 | P4 | 与 headless / `/diff` 命令共用 `format_summary` | 两处输出文本一致 |

#### D. #79 子 Agent 护栏（现有 5 条，补 7 条）

| ID | 类别 | 用例 | 断言要点 |
|---|---|---|---|
| SA-06 | P1 | 批量规模内放行 | `count <= max_batch` → 通过 |
| SA-07 | P3 | 批量边界 | `count == max_batch` 通过；`max_batch + 1` 拒绝 |
| SA-08 | P1 | run 累计上限 | 累计达 `max_total` 后即使单批合法也拒绝 |
| SA-09 | P2 | 拒绝信息可诊断 | 含 `max_total / launched / remaining / requested` |
| SA-10 | **P1** | **并发不超发** | 多线程同时 `try_reserve` → 总配额不超（预算是共享可变状态，原子性必须验） |
| SA-11 | P2 | 拒绝不中断主流程 | 返回错误文本而非崩溃/抛异常 |
| SA-12 | P3 | 未配置 / `max_total = 0` 默认值行为 | 明确且不崩溃 |
| SA-13 | **P4** | **防递归回归** | 子 Agent 工具集**不含** AgentTool → 嵌套深度恒为 1。这是 `agent_tool.cpp:80/103` 的 `continue` 守卫，必须有回归用例锁住，防止后续重构误删 |

> 📌 **对 Issue #79 标题的修正**：标题写的是「递归深度上限」，但深度其实已被**结构性禁止**——`agent_tool.cpp:80`（develop）/ `:103`（#79 分支）构建子 Agent 工具集时无条件 `continue` 掉 `kAgentToolName`，嵌套深度恒为 1。真实缺口是**横向规模**（批量 + run 累计），护栏实现也是按这个做的。因此**不需要**再写深度上限用例，SA-13 改为「锁住防递归守卫」的回归用例。
> 📌 这条同时说明：本仓库的 issue 描述会过时，**动手前必须回代码核实**（此前 #52 / #87 / #89 均已出现同类情况）。

#### E. #78 验证闭环（~~未实现~~ → ~~未接入默认路径~~ → **P1 / P2 已落地**）

> ✅ **三次更新（2026-09-30 18:39，`9a2de19` / PR #98）**：原列的三项缺口，前两项已闭合 ——
> **① 接入默认 ReAct 路径** ✅ `run_verification_gate()` 已进入 `react_loop.cpp` 主循环（FinalAnswer 落地之前）；
> **② 默认开启而非手工 opt-in** ⚠️ 部分 —— headless 默认开（`enabled_by_default=true`，评测入口先受益），**交互式仍默认关**；
> **③ 与预算（#55）联动** ❌ 未做（设计文档列为 P4）。
> 完整落地说明见 `docs/design-issue-78-verification-loop.md`。
>
> 🔴 **事实更正（二次）**：#78 不是「从零实现」。`#31/#32` 已留下目标验证栈，且**已有测试**：
> `goal_verdict.h`（`AgentGoal` / `GoalStatus`）、`verdict.h`（`check_goal` / `guard_command` / `parse_goal`）、
> `GoalGuardedAgent`（`agent_loop_adapters.cpp:33`，经 `agent.type="goal-guarded"|"verify"` 启用，`app_config.cpp:129`）。
> 实现走的是**接线 + 改默认值**，工作量与风险显著低于初版估计。

| ID | 类别 | 规格 | 现状 |
|---|---|---|---|
| VF-01 | P1 | 未通过验证（编译/测试/guard_command）不得进入 FinalAnswer | ✅ **已落地** —— 用例「VF-01: 验证未通过时不产生无警告的最终答复」 |
| VF-02 | P1 | 验证失败 → 错误信息回灌 LLM 重试；重试次数达上限后降级为「带警告终止」 | ✅ **已落地**（3 条用例）—— 含「回灌必须消耗预算（不得静默死循环）」，锁住 `continue` 跳过预算记账的坑 |
| VF-03 | P2 | 项目无可用验证命令（无 CMake/Makefile）→ 跳过而非失败 | ✅ **已落地**（门禁侧，用例「无可用验证器时门禁直接放行」）；⚠️ **命令自适应探测**（区分硬编码 `kLintCmd` 与真实探测）仍归 P4 |

**建议补充（已随 VF-01~03 一并回帖到 issue #78）**：

| ID | 类别 | 用例 | 理由 |
|---|---|---|---|
| **VF-04** | P1 | **默认路径接入**：脚本化模型全程不调工具直接给 final_answer，断言退出前发生了验证判定 | ✅ **已落地** —— 本 issue 的真缺口已闭合（用例「VF-04: apply_verification_gate 按入口正确装配门禁」） |
| **VF-05** | P2 | `Stop` hook 的 `blockingError` 不得吞掉 final_answer（`react_loop.cpp:1218-1224` 的覆写） | ✅ **已落地**（用例「VF-05: Stop hook 阻断不吞掉验证结论」）—— 门禁不走 Stop 路线，天然绕开该覆写 |
| **VF-06** | **P1·安全** | `guard_command` 必须拦住 `goal.command` 注入：`;` `&&` `\|` `$()` 反引号、换行 | ✅ **已落地**（2 条用例：字符串级拦截 + 正常调用形式不误伤）—— 自动执行场景的 RCE 前置已守住 |
| **VF-07** | P2 | 「无可用验证命令」的探测行为（`custom_script` + 项目无 CMake/Makefile → 跳过） | ⚠️ **部分** —— 用例锁了「目标类型与 checker 对应」；真实探测语义归 P4 |
| **VF-08** | P4 | headless(#77) × 验证闭环：验证执行时不得阻塞 stdin（无人值守） | ❌ **未落地** —— 依赖 #77 的 headless 集成基建（与 HL-12 同类），待同批补 |
| **VF-09** | P2 | 达 `max_iterations`（`at_limit`）时是否仍能完成一轮验证 | ✅ **已落地**（用例「VF-09: 预算仅剩一轮时不再无效回灌，直接降级」）；⚠️ 内部评审器两条退出路径仍不经门禁（设计文档 §7.1） |

> ⚠️ **实现约束（影响方案选型）**：`Stop` 事件**撑不起闭环**——派发点在 `react_loop.cpp` 循环收尾之后，
> `blockingError` 仅覆写 final_answer，**不会重新进入循环**。单靠 `Stop` 只能「注入一句警告」，
> 做不到「验证 → 失败 → 回灌 → 修复 → 重验」。
> → ✅ **落地采用的方案**：在 loop 内部新增退出前判定点（`run_verification_gate()`，位于 FinalAnswer 落地之前）；
> **未**采用 `GoalGuardedAgent` 外层包壳——headless 自行构造 `ReActLoop`，走包壳会让评测入口零验证。

三条先以规格形式挂在 #78（✅ **已于 2026-09-30 回帖**），实现落地后在 `tests/unit/agent/core/test_react_loop.cpp` 用脚本化模型实现。
> ✅ **已实现**（2026-09-30，PR #98）：13 条 `[issue78]` 用例分布在 `test_react_loop.cpp`（8 条）与 `test_agent_core.cpp`（5 条）。
> 未新增独立的 `scripted_model.h` —— 直接复用了 `VerificationGateFixture` + `MockCompletionProvider` 的多轮脚本能力。

> 📌 前置：`scripted_model.h`（§0.3 第 3 项）为 VF-02 / VF-04 的多轮脚本化所必需 → ✅ 落地时以 `VerificationGateFixture` 等价实现，**无需新增基建**。
> 📌 既有 goal/verdict 用例标签为 `[agent][goal][verify]`；新用例统一打 `[issue78]`，已满足 §2.3 规范，可 `ctest -L issue78` 单 issue 回归。
> ⚠️ **不要**用 `[#78]` 形式 —— `#` 开头是 Catch2 保留的文件名标签，会导致用例无法被按名选中。

### 1.3 明确不覆盖的场景（及原因）

| # | 不覆盖 | 原因 |
|---|---|---|
| 1 | 真实 LLM 调用 | 非确定性、需 API Key、慢、成本高 → 用 `MockCompletionProvider` 脚本化替代（已有基建，秒级反馈） |
| 2 | 网络依赖（远端 MCP server / WebFetch / WebSearch） | 已有 `tests/unit/agent/mcp/fake_*_server.py` 与 `web_live_probe.cxx`（明确标注不入 CI），单测层不重复 |
| 3 | TUI 渲染与交互 | headless 设计上就不依赖 TUI；TUI 侧已有 `tui_unit_tests` |
| 4 | git 自身行为（rename 检测算法、diff 精度） | 测 Workx 的**归一化与统计**逻辑，不测 git。用固定 fixture 保证确定性 |
| 5 | 性能 / 基准 | schema 校验 O(n)、大仓 diff 耗时 → 归 `tests/benchmarks`（默认 OFF） |
| 6 | 平台特有行为（Windows 沙箱降级、命令过滤） | 属 #84 / #85（P1），不在本次 P0 范围；且 CI 现为 Linux runner |
| 7 | 已被既有测试覆盖的部分（session store、tool registry 基础行为、token 统计） | 避免重复投入 |
| 8 | 覆盖率门禁 | 仓库无 gcov/lcov 基建；先做「用例存在性 + 全绿」门禁，覆盖率作为后续软报告 |

---

## 2. 存放位置

### 2.1 分层策略

| 层 | 位置 | 构建 | 适用 | 判据 |
|---|---|---|---|---|
| **L1 单元** | `tests/unit/**` | 默认 ON | 纯函数、无 IO、无子进程 | schema 校验、budget 计数、exit_code 派生、permission_mode 解析、路径归一化 |
| **L2 组件** | `tests/unit/**`（用 `mock_provider` 驱动 ReActLoop） | 默认 ON | 无真模型、无真网络、可含临时文件 | headless 主流程、schema 接入与自纠 |
| **L3 集成** | `tests/integration/**` | `WORKX_BUILD_INTEGRATION_TESTS=ON` | 真子进程 / 真 git 仓库 / HTTP fixture | git 仓库真实 IO、CLI 子进程 |
| **L4 端到端** | `tests/e2e/**`（新建，可选） | 独立 option | 编译产物 `workx -p "..."` 真实跑 | 冒烟 + Harbor 样例题 |

**关于 #81 真 git 用例的归属（需决策）**：
造临时仓库要 fork `git` 子进程、依赖 PATH 上的 git、且 Windows/Linux 行为有差异。两个选择：

- **方案 A（推荐）**：留在 `tests/unit`，用 Catch2 的 `SKIP()` 守卫（`git --version` 不可用则跳过），打 `[git]` 标签。优点：默认会被跑，不需要新目标；缺点：临时仓库 IO 会略微拖慢 unit。
- **方案 B**：搬去 `tests/integration`。缺点：该目标默认 OFF，等于没人跑，违背「默认 OFF 是怕 LM Studio 缺失」的原意（git 用例并不需要 LM Studio）。

建议 **A**，并把纯逻辑（`short_sha` / `normalize_numstat_path` / `format_summary`）与真仓库用例拆成两个文件，前者无 IO、快。

### 2.2 目录结构

```
tests/
├── unit/                                   # L1 + L2，glob 自动收集，无需改 CMake
│   ├── agent/
│   │   ├── headless/
│   │   │   └── test_headless.cpp           # #77  L2：mock backend 驱动主流程
│   │   │   └── test_headless_output.cpp    # #77  L1：输出序列化纯函数（可选拆出）
│   │   ├── util/
│   │   │   ├── test_json_schema.cpp        # #80  已存在（8 条）
│   │   │   ├── test_json_schema_wiring.cpp # #80  新增：JS-16 错误回灌自纠（JS-15 已由 test_json_schema.cpp:160 覆盖）
│   │   │   ├── test_git_checkpoint.cpp     # #81  已存在（分支上）纯逻辑
│   │   │   └── test_git_checkpoint_repo.cpp# #81  新增：真仓库，[git] 标签
│   │   ├── tool/
│   │   │   └── test_sub_agent_budget.cpp   # #79  已存在（分支上）5 条
│   │   └── core/
│   │       └── test_react_loop.cpp         # #78  VF-01~07 / VF-09 已在此实现（8 条）
│   └── helpers/
│       ├── mock_provider.h                 # 已存在
│       ├── mock_git_repo.h                 # 【新增】RAII 临时仓库 fixture
│       └── scripted_model.h                # 【新增·可选】脚本化模型 DSL
├── integration/                            # L3，option 控制
│   └── test_headless_cli.cpp               # 真 CLI 子进程 + 假 HTTP 后端
└── e2e/                                    # L4，新建（可选）
    ├── test_headless_e2e.cpp
    └── tasks/                              # Harbor 格式样例题（后续）
```

**与 src 的映射规则**：`src/<模块>/<文件>.cpp` ↔ `tests/unit/<模块>/test_<文件>.cpp`，路径逐层对齐。
例：`src/agent/headless/headless.cpp` → `tests/unit/agent/headless/test_headless.cpp`。
`src/agent/util/git_checkpoint.cpp` → `tests/unit/agent/util/test_git_checkpoint.cpp`。

### 2.3 文件与用例命名规范

**文件**：`test_<被测单元>.cpp`，全小写 + 下划线；一个被测类/模块一个文件；纯逻辑与 IO 用例分文件（便于标签过滤）。

**TEST_CASE**：

```cpp
TEST_CASE("headless: 未知权限模式返回 exit 2", "[headless][error][issue77]") {
    // 对应 src/agent/headless/headless.cpp:212-218
    ...
}
```

- 名称：`<模块>: <具体行为>`，用行为描述而非函数名（函数重命名时用例名不失效）
- 标签（必填三类）：`[<模块>]` + `[<类别>]` + `[#issue号]`

| 标签 | 含义 | ctest 用法 |
|---|---|---|
| `[core]` | 核心主流程 | `ctest -L core` 冒烟子集 |
| `[error]` | 异常分支 | |
| `[boundary]` | 边界条件 | |
| `[regression]` | 回归/跨模块 | |
| `[issue77]` `[issue78]` `[issue79]` `[issue80]` `[issue81]` | 按 issue 聚合 | `ctest -L issue77` 单 issue 回归 |
| `[slow]` | 慢（>1s，如并发压测） | `ctest -LE slow` 快子集 |
| `[git]` | 依赖外部 git 可执行文件 | 环境缺失时 SKIP |
| `[.]` / `[!shouldfail]` | 隐藏 / 预期失败（待实现） | 默认不跑 |

> 标签会被 `ADD_TAGS_AS_LABELS ON` 自动映射为 ctest label，无需改 CMake。

**每个 TEST_CASE 顶部必须写 3 行注释**：对应 issue、覆盖的源码位置（文件:行号）、失败时能定位到什么。

---

## 3. 落地步骤

### 3.1 执行顺序

```
阶段 0 ✅  →  阶段 1 ✅  →  阶段 2 ✅
                                    ↓
     阶段 3（0.5d）#79  →  阶段 4（0.5d）#81  →  阶段 5（#78 剩余 P3/P4/VF-08）
                                    ↓
                阶段 6（0.5d）CI 接入  ← ✅ 已落地（观察期软门禁）
```

> **五次更新（2026-10-01）**：阶段 0 / 1 / 2 **已全部完成**（PR #100 `241f014` + PR #101 `edd2608`）。
> **阶段 6（CI 接入）已落地** —— 新增 `.github/workflows/build-test.yml`，并在 WSL 上按真实 CI 步骤预演（编译全通）；
> 首轮暴露 5 类 Linux 平台缺陷（#104 / #105 / #107 / #109），门禁先以 `continue-on-error` 观察，待其关闭后转硬。

| 阶段 | 内容 | 状态 | 产出 |
|---|---|---|---|
| **0** | ① `helpers/mock_git_repo.h` ② headless 注入口 `headless_internal.h` ③（可选）`scripted_model.h` | ✅ **已完成**（PR #100；③ 未做） | 可测性基建 |
| **1** | HL-01 ~ HL-12 | ✅ **已完成**（`test_headless.cpp`，13 TEST_CASE，PR #100） | #77 零覆盖 → 13 条 |
| **2** | JS-15 / JS-16 | ✅ **已完成**（`test_json_schema_wiring.cpp`，PR #101） | #80 端到端闭环 |
| **3** | SA-06 ~ SA-13 | ⬜ 待做（#79 已合入 `67ea633`，仅需补边界与回归） | #79 护栏完整 |
| **4** | GC-08 ~ GC-15 | ⬜ 待做（#81 已合入 `b2d41ca`；`mock_git_repo.h` 已就绪） | #81 完整 |
| **5** | VF-08 / P3 / P4 | ⬜ 部分待做（#78 P1/P2 已落地 `9a2de19`，13 条 `[issue78]` 全绿） | 验证闭环收尾 |
| **6** | `.github/workflows/build-test.yml` | ✅ **已落地**（观察期软门禁：编译全通，23/1342 平台缺陷待清） | CI 门禁 |
| **6.1** | 清 #105 / #104 / #107 / #109 | 🔴 **新增最高优先**（CI 转硬门禁的前置） | 门禁由软转硬 |

**排序理由（五次更新后）**：
- **新增最高优先：清 #105 / #104 / #107 / #109** —— 阶段 6 一落地就暴露出这批平台缺陷；
  它们此前从未被发现（CI 从未跑过测试，本机开发走 Windows 不命中），
  而 #105（island IPC 缺 `shutdown()`）会让 `ctest` **挂死**，是转硬门禁的关键阻塞项；
- 阶段 3 / 4 其次 —— 实现与基础用例已在 develop，只需补边界与回归，投入小；
- 阶段 5 最后 —— #78 的 P1/P2 已落地，仅剩 VF-08 与 P3/P4 设计项。

**建议排布**：先修 #105（一行改动即可消掉 12 项 hang）→ #104 → #107 → #109，随后把 CI 由软转硬；
阶段 3 / 4 / 5 可与上述缺陷清理并行。

### 3.2 CI 接入 ✅ 已落地

**现状问题（已解决）**：`code-quality.yml` 只有 lint/格式/复杂度/cppcheck，`release.yml` 只 build 不测试 → **CI 从来没有跑过任何一个测试用例**。

**新增 `.github/workflows/build-test.yml`**（触发条件与 `code-quality.yml` 对齐）：

| 设计点 | 取值 | 理由 |
|---|---|---|
| runner | `ubuntu-latest` | Windows 上 ctest 因中文测试名编码不可用（#106）；且同等规模构建快 2–3 倍 |
| 构建配置 | Release + Ninja | 与 vcpkg `x64-linux` 预编译依赖的配置一致 |
| 构建目标 | 显式列出 core/agent/island/tui 四个 `*_unit_tests` | `cmake --build build` 会连带构建 workx 主程序与 example，与验证测试无关 |
| 依赖裁剪 | `WORKX_WITH_TREE_SITTER=OFF`、`WORKX_BUILD_EXAMPLES=OFF` | 去掉 FetchContent 网络依赖与无关目标 |
| 缓存 | `vcpkg` + `build/vcpkg_installed` | 只缓存前者会导致每轮重编 curl/openssl，比不缓存更慢 |
| 用例选择 | `ctest -LE slow` | 跳过 3 个依赖 30s 兜底的 `[slow]` 用例 |
| 三重防御 | `--no-tests=error` + **用例数下限 1200** + `--timeout 180` | 分别防「零发现」「部分丢失」（与 #106 同源风险）「hang 挂死」（#105） |
| 门禁强度 | **观察期软门禁**（`continue-on-error: true`） | 首轮实测 23/1342 失败，全部由下文记录的平台缺陷造成 |

**WSL 真机预演结论**（Ubuntu 24.04 + 全新 vcpkg + Ninja + Release，完整复刻 CI 步骤）：

- ✅ configure + 编译 4 个测试目标**全部成功**（含此前在本机编不过的 `tui_unit_tests`）
- ❌ `ctest -LE slow --timeout 60`：**98% passed，23 failed out of 1342**

| 失败类别 | 数 | Issue |
|---|---|---|
| island Timeout（hang） | 12 | #105 `transport_posix.cpp` 的 `close()` 缺 `shutdown()` |
| MCP SIGPIPE | 4 | #104 连接失败路径写已关闭管道 |
| GrepTool Failed | 5 | #107 搜索类用例在 Linux 上返回错误 |
| windows drive pattern | 1 | #109 缺平台守卫 |
| subprocess | 1 | #109 退出码断言平台相关 |

> 另有 6 处测试硬编码 `python`（Ubuntu 只有 `python3`），已并入 #104。

**转硬门禁的条件**：#104 / #105 / #107 / #109 关闭后，删除 `continue-on-error: true` 一行即可。

**三个原计划预判的坑 —— 均已规避**：
1. **git 身份** ✅ `tests/unit/helpers/mock_git_repo.h` 的 fixture commit 已带 `-c user.name` / `-c user.email`
2. **Windows/Linux 差异** ✅ 门禁平台确定为 Linux；Windows 侧另因 ctest 编码问题（#106）暂不纳入
3. **不要直接开硬门禁** ✅ 用 `continue-on-error` 显式表达「观察期」，而非 `|| true` 的静默假绿；同时补了用例数下限守卫，防止「用例变少但依然绿」

### 3.3 提交规范

**分支命名**（与仓库现有 `fix/issue-77-80-headless-and-schema`、`feat/issue-81-git-checkpoint` 对齐）：

```
test/issue-<号>-<主题>
例：test/issue-77-headless、test/issue-80-schema-wiring
```

**提交信息**（沿用 Conventional Commits + issue 号）：

```
test(#77): 补充 headless 输出格式与退出码用例

- 新增 tests/unit/agent/headless/test_headless.cpp（HL-01~HL-12）
- 抽出 run_headless_with_provider 以支持注入 MockCompletionProvider
- 覆盖 text/json/stream-json 三种输出格式与 exit_code 0/1/2 三条分支

Refs #77
```

**PR 规范**：
- 标题：`[Test] <主题>`
- 关联写法：#77 / #79 / #80 / #81 均已 CLOSED，统一用 **`Refs #<号>`**，不要用 `Closes`（会把已关闭的 issue 重开或造成语义混乱）
- body 必填：新增用例 ID 清单、覆盖矩阵（哪条用例对哪条验收标准）、未覆盖项及原因

**代码门禁**（新增文件自动受 `code-quality.yml` 约束）：
- 格式：`clang-format==18.1.8` + 仓库 `.clang-format`（Google 风格 / 4 空格 / 100 列）→ 提交前本地 `clang-format -i`
- 复杂度：lizard `CCN ≤ 15` 且 `函数 ≤ 50 行` → 测试函数也要拆，别写一个 200 行的 TEST_CASE
- 不要绕过门禁：`--no-verify` 禁用

---

## 附录：验收标准（本案完成时）

> 五次更新（2026-10-01）：#77 / #80 已随 PR #100 / #101 完成；**CI 接入已落地**（观察期软门禁）。

- [x] #77 用例数 ≥ 12 —— ✅ `test_headless.cpp` 13 条，HL-01~HL-12（PR #100 `241f014`）
- [x] headless 可测性改造 —— ✅ `headless_internal.h` 暴露 `run_headless_with_provider()`（PR #100）
- [x] #80 补齐 JS-16 —— ✅ `test_json_schema_wiring.cpp` 覆盖 JS-15 + JS-16（PR #101 `edd2608`）
- [x] `helpers/mock_git_repo.h` —— ✅ 已建（PR #100，95 行）
- [ ] #79 补齐 SA-06 ~ SA-13（含并发不超发 SA-10、防递归回归 SA-13）
- [ ] #81 补齐 GC-08 ~ GC-15（含真仓库用例与 `[git]` 标签守卫）
- [x] #78 验收规格已挂到 issue（2026-09-30 已回帖）
- [x] #78 实现已落地（P1 / P2，`9a2de19` / PR #98）——VF-01 ~ VF-07 / VF-09 共 13 条用例全绿
- [ ] VF-08（headless 下验证执行不阻塞 stdin）+ P3 / P4
- [x] **CI 存在 build + test job，且对 develop 的 push/PR 生效** —— ✅ `.github/workflows/build-test.yml`
      （ubuntu + `ctest -LE slow`；WSL 真机预演：编译全通，98% passed / 23 failed，先以 `continue-on-error` 观察）
- [x] 无一个用例依赖真实 LLM 或外网（`-LE slow` 子集；`[live]` 用例在无用户配置时自动跳过）
- [ ] 🔴 **清 #105 / #104 / #107 / #109**，使 Linux 上 `ctest -LE slow` 全绿，随后删除
      `continue-on-error: true` 转为硬门禁
- [x] 评分卡分数同步（`docs/agent-harness-scorecard.html` 保持 **71.9**；CI 接入属工程化基建，不改 harness 能力评分）
