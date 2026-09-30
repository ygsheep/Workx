# P0 功能测试用例方案

> 适用范围：Issue #77 ~ #81（Agent Harness 评分卡中的 5 项 P0）
> 编写日期：2026-09-30 · **复审更新 2026-09-30 12:30** · **二次更正 2026-09-30 13:20**（develop `b2d41ca`）
> 状态：方案待评审；#77 / #79 / #80 / #81 均已合入 develop，可立即开工；#78 已有验证原语（#31/#32）但**默认路径未接线**
>
> 🔴 **二次更正说明**：初版与复审均把 #78 判为「代码未实现 / 零实现」，**该判定不成立**。详见 §1.2 E。

---

## 0. 前提假设与事实校正

### 0.1 实现状态（复审已更新到 commit 级）

> 📌 本方案初版编写时 #79 / #81 尚未合入。**现均已合并到 develop**，下表为复审后的实际情况。
> 📌 **二次更正**：#78 行的「现有测试：无 / ❌ 需先实现」是错的。`#31/#32` 的目标验证栈已存在**且有测试**（`test_agent_core.cpp:95-111`），
> 缺的是**接入默认 ReAct 路径 + 默认开启**。本 issue 的性质是「接线 + 改默认值」，不是「从零建验证器」。

| Issue | 功能 | GitHub 状态 | develop 合并提交 | 现有测试 | 可开工 |
|---|---|---|---|---|---|
| #77 | headless 非交互模式 | CLOSED | `d34d44a`（PR #90） | ❌ **零覆盖** | ✅ 最高优先 |
| #80 | json_schema 通用参数校验 | CLOSED | `d34d44a`（PR #90） | ⚠️ 8 用例，`test_json_schema.cpp` | ✅ |
| #79 | 子 Agent 派生护栏 | CLOSED | `67ea633`（PR #92） | 5 用例，`test_sub_agent_budget.cpp` | ✅ |
| #81 | git 检查点与回滚 | CLOSED | `b2d41ca`（PR #93） | 7 用例，`test_git_checkpoint.cpp` | ✅ |
| #78 | 强制验证闭环 / PreCompletion 门禁 | **OPEN** | ❌ 无 | ⚠️ 已有 `goal_verdict` / `verdict` 用例（`test_agent_core.cpp:95-111`，**缺 `[#78]` 标签**） | ⚠️ **部分可开工**：VF-03 / VF-06 / VF-07 现在就能写 |

**结论**：4 项可立即开工，**#77 是唯一零覆盖项且优先级最高**；#78 **不是**「代码未实现」——验证原语已存在，缺的是接线，因此 **VF-03 / VF-06 / VF-07 可立即开工**，VF-01 / VF-02 / VF-04 需先接入默认路径。

**复审附带结论（评分同步更新）**：四项 P0 落地后，Harness 加权总分从 **61.05 → 68.5**（详见 `docs/agent-harness-scorecard.html`）。初评「补齐 P0 后约 82 分」的预估偏乐观——它把 #78（权重 20% 的最大项）算进了红利，而这一项在**默认路径上**完全没生效（验证原语存在但未接线，见 §1.2 E）。

**新暴露的三个风险**（P0 落地后才出现，已计入本方案）：
1. **headless 零测试** —— 唯一合入即零覆盖的 P0 模块，失败模式是「静默输出错内容」而非崩溃；
2. **headless 无法注入 mock 后端** —— 阻塞 HL 用例主流程（见 §0.3）；
3. **#80 只测了纯函数** —— 未覆盖「执行侧真的接入」与「错误回灌自纠」，等于验证了一半。

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
| CI | `.github/workflows/code-quality.yml` 只有 complexity / format / clang-tidy / cppcheck 四个 job；`release.yml` 有 build 但**不跑测试** → **当前 CI 完全没有 build/test job** |

### 0.3 需要新增的基建（否则部分用例写不了）

1. **`mock_git_repo.h`**：RAII 临时 git 仓库 fixture（init / commit / 改 / 删 / 未跟踪 / rename）。#81 的真仓库用例需要。
2. **headless 后端注入口**：`run_headless()` 内部直接调 `create_backend(cfg, ...)` 建真实后端，**测试无法注入 `MockCompletionProvider`**。需抽出内部重载（如 `run_headless_with_provider(cfg, task_manager, provider, opts)`），否则 #77 只能测输出序列化纯函数，测不到主流程。
   → 这是 #77 用例能否落地的**阻塞项**，建议与用例同 PR 提交。
3. **`scripted_model.h`（可选）**：把「第 N 轮返回指定 tool_call / 文本」封装成一行 DSL，供 #78 落地后大规模复用。

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
| JS-15 | **P1** | **执行侧真的调用了校验** | 通过 `ToolRegistry` 执行一次带错参的工具调用 → 返回 schema 错误，**不落到工具实现**。当前 8 条只测了纯函数，没测「接入」，等于 #80 只做了一半 |
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

#### E. #78 验证闭环（~~未实现~~ → **能力已存在，默认路径未接线**）

> 🔴 **事实更正**：#78 不是「从零实现」。`#31/#32` 已留下目标验证栈，且**已有测试**：
> `goal_verdict.h`（`AgentGoal` / `GoalStatus`）、`verdict.h`（`check_goal` / `guard_command` / `parse_goal`）、
> `GoalGuardedAgent`（`agent_loop_adapters.cpp:33`，经 `agent.type="goal-guarded"|"verify"` 启用，`app_config.cpp:129`）。
> 缺的是三件事：**① 接入默认 ReAct 路径**（`react_loop.cpp` 对 `check_goal` / `AgentGoal` / `has_checker` **零命中**）
> **② 默认开启而非手工 opt-in**（默认 `agent.type` 为空 → 纯 ReAct → 零验证）**③ 与预算（#55）联动**。
> → #78 的性质是**接线 + 改默认值**，工作量与风险显著低于初版估计。

| ID | 类别 | 规格 | 现状 |
|---|---|---|---|
| VF-01 | P1 | 未通过验证（编译/测试/guard_command）不得进入 FinalAnswer | ❌ 尚未接入默认路径 |
| VF-02 | P1 | 验证失败 → 错误信息回灌 LLM 重试；重试次数达上限后降级为「带警告终止」 | ⚠️ `GoalGuardedAgent` 已有 `max_attempts=50` 重试语义，但非默认 |
| VF-03 | P2 | 项目无可用验证命令（无 CMake/Makefile）→ 跳过而非失败 | ⚠️ 需澄清：现行 `kLintCmd="echo 'no lint config'"`（`verdict.cpp:289`）是**硬编码默认命令**，与本条要的「探测项目是否真有命令」语义不同，不能直接宣称已覆盖 |

**建议补充（已随 VF-01~03 一并回帖到 issue #78）**：

| ID | 类别 | 用例 | 理由 |
|---|---|---|---|
| **VF-04** | P1 | **默认路径接入**：脚本化模型全程不调工具直接给 final_answer，断言退出前发生了验证判定 | 本 issue 的真缺口；唯一能证明「闭环真的关上了」的用例 |
| **VF-05** | P2 | `Stop` hook 的 `blockingError` 不得吞掉 final_answer（`react_loop.cpp:1218-1224` 的覆写） | 该覆写可能是缺陷；走 Stop 路线会直接踩到 |
| **VF-06** | **P1·安全** | `guard_command` 必须拦住 `goal.command` 注入：`;` `&&` `\|` `$()` 反引号、换行 | 自动执行场景的 RCE 前置，**先于功能本体落地** |
| **VF-07** | P2 | 「无可用验证命令」的探测行为（`custom_script` + 项目无 CMake/Makefile → 跳过） | 补 VF-03 与 `kLintCmd` 之间的语义差 |
| **VF-08** | P4 | headless(#77) × 验证闭环：验证执行时不得阻塞 stdin（无人值守） | 跨模块耦合，与 HL-12 同类 |
| **VF-09** | P2 | 达 `max_iterations`（`at_limit`）时是否仍能完成一轮验证 | 呼应 issue 关联的 #55 |

> ⚠️ **实现约束（影响方案选型）**：`Stop` 事件**撑不起闭环**——派发点在 `react_loop.cpp:1201-1227`，位于**循环收尾之后**，
> `blockingError` 仅覆写 final_answer（`:1218-1224`），**不会重新进入循环**。单靠 `Stop` 只能「注入一句警告」，
> 做不到「验证 → 失败 → 回灌 → 修复 → 重验」。必须在 loop 内部新增退出前判定点，或走 `GoalGuardedAgent` 外层包壳。

三条先以规格形式挂在 #78（✅ **已于 2026-09-30 回帖**），实现落地后在 `tests/unit/agent/core/test_react_loop.cpp` 用脚本化模型实现。

> 📌 前置：`scripted_model.h`（§0.3 第 3 项）为 VF-02 / VF-04 的多轮脚本化所必需，建议从「可选」提升为**必需项**。
> 📌 现有 goal/verdict 用例标签为 `[agent][goal][verify]`，**缺 `[#78]`**，不满足 §2.3 自订规范，无法 `ctest -L "#78"` 单 issue 回归。

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
│   │   │   ├── test_json_schema_wiring.cpp # #80  新增：JS-15/16 执行侧接入
│   │   │   ├── test_git_checkpoint.cpp     # #81  已存在（分支上）纯逻辑
│   │   │   └── test_git_checkpoint_repo.cpp# #81  新增：真仓库，[git] 标签
│   │   ├── tool/
│   │   │   └── test_sub_agent_budget.cpp   # #79  已存在（分支上）5 条
│   │   └── core/
│   │       └── test_react_loop.cpp         # #78  落地后在此加 VF-01~03
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
TEST_CASE("headless: 未知权限模式返回 exit 2", "[headless][error][#77]") {
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
| `[#77]` `[#78]` `[#79]` `[#80]` `[#81]` | 按 issue 聚合 | `ctest -L "#77"` 单 issue 回归 |
| `[slow]` | 慢（>1s，如并发压测） | `ctest -LE slow` 快子集 |
| `[git]` | 依赖外部 git 可执行文件 | 环境缺失时 SKIP |
| `[.]` / `[!shouldfail]` | 隐藏 / 预期失败（待实现） | 默认不跑 |

> 标签会被 `ADD_TAGS_AS_LABELS ON` 自动映射为 ctest label，无需改 CMake。

**每个 TEST_CASE 顶部必须写 3 行注释**：对应 issue、覆盖的源码位置（文件:行号）、失败时能定位到什么。

---

## 3. 落地步骤

### 3.1 执行顺序

```
阶段 0（0.5d）基建  →  阶段 1（1.5d）#77  →  阶段 2（0.5d）#80
                                    ↓
              阶段 3（0.5d）#79  →  阶段 4（0.5d）#81  →  阶段 5（#78 实现后）
                                    ↓
                          阶段 6（0.5d）CI 接入
```

> 复审更新：#79 / #81 已合入 develop，**阶段 3 / 4 的前置条件已解除**，可与阶段 1、2 并行推进。

| 阶段 | 内容 | 前置 | 产出 |
|---|---|---|---|
| **0** | ① `helpers/mock_git_repo.h` ② headless 后端注入重载 ③（可选）`scripted_model.h` | 无 | 可测性基建 |
| **1** | HL-01 ~ HL-12 | 阶段 0 ② | #77 从零覆盖 → 12 条 |
| **2** | JS-09 ~ JS-16（重点 JS-15/JS-16） | 无 | #80 补齐执行侧接入验证 |
| **3** | SA-06 ~ SA-13 | ✅ 无（#79 已合入 `67ea633`） | #79 护栏完整 |
| **4** | GC-08 ~ GC-15 | ✅ 无（#81 已合入 `b2d41ca`） | #81 完整 |
| **5** | VF-01 ~ VF-09 | ⚠️ #78 需先接入默认路径（VF-03 / VF-06 / VF-07 不依赖接线，可先行） | 验证闭环 |
| **6** | `.github/workflows/build-test.yml` | 阶段 1 完成 | CI 真门禁 |

**排序理由**：
- #77 第一 —— 唯一零覆盖，且是整条评测链路的入口，失败模式是「静默错」最难发现；
- #80 第二 —— JS-15/JS-16 是 issue 本意（执行侧校验），现有 8 条只覆盖了一半，补它最便宜；
- #79 / #81 第三/四 —— 实现与基础用例已在 develop，只需补边界与回归；
- #78 最后 —— **默认路径未接线**（验证原语已存在）；其中 VF-03 / VF-06 / VF-07 不依赖接线，可与阶段 1 并行。

**建议并行排布**：阶段 0 的 ① 与 ② 可并行（互不依赖），阶段 1 开工的同时阶段 3 / 4 可由另一人推进。

### 3.2 CI 接入

**现状问题**：`code-quality.yml` 只有 lint/格式/复杂度/cppcheck，`release.yml` 只 build 不测试 → **CI 从来没有跑过任何一个测试用例**。这不是加几个 job 的问题，是门禁缺失。

**新增 `.github/workflows/build-test.yml`**（与 `code-quality.yml` 的触发条件对齐）：

```yaml
name: Build & Test

on:
  push:
    branches: [develop]
  pull_request:
    branches: [develop]

jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      # vcpkg bootstrap + cache：直接复用 release.yml 中已验证的段落
      - name: Configure
        run: cmake --preset default -DWORKX_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
      - name: Build
        run: cmake --build build --config Release -j 4
      - name: Run unit tests (fast subset)
        run: ctest --test-dir build -C Release -LE slow -j 4 --output-on-failure

  slow-tests:
    runs-on: ubuntu-latest
    continue-on-error: true   # 先观察 flaky，稳定后转硬门禁
    steps:
      # ... 同上 build ...
      - run: ctest --test-dir build -C Release -L slow --output-on-failure
```

**门禁分级**（沿用 `code-quality.yml` 已确立的「硬门禁只卡新增、软报告看全量」策略，避免重蹈 `|| true` 假绿覆辙）：

| 级别 | 内容 | 阻塞 |
|---|---|---|
| 硬门禁 | `ctest -LE slow` 全绿；新增 P0 用例必须存在且通过 | ✅ |
| 硬门禁 | 单 issue 回归：`ctest -L "#77"` 等 | ✅（阶段 1 后开启） |
| 软报告 | `-L slow`（并发压测等），`continue-on-error: true` 观察 2 周再转硬 | ❌ |
| 软报告 | 全量测试耗时 / 通过数趋势，上传 artifact | ❌ |

**三个必须注意的坑**：
1. **git 身份**：fixture 里 commit 必须带 `-c user.name=... -c user.email=...`，否则 CI 环境 `git commit` 失败 → `[git]` 用例全红。
2. **Windows/Linux 差异**：`[git]` 用例在 Windows 上受 CRLF（`core.autocrlf`）影响（项目记忆里已有同类踩坑），建议先只在 Linux runner 跑，Windows 作为后续软报告。
3. **前 2 周不要直接开硬门禁**：先 `continue-on-error` 跑，确认无 flaky 再转。否则团队会像注释里写的那样「关掉整个 workflow」。

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

- [ ] #77 用例数 ≥ 12，且能 `ctest -L "#77"` 单独跑通（**当前 0，最高优先**）
- [ ] #80 补齐 JS-15 / JS-16（执行侧接入验证 + 错误回灌自纠）
- [ ] #79 补齐 SA-06 ~ SA-13（含并发不超发 SA-10、防递归回归 SA-13）
- [ ] #81 补齐 GC-08 ~ GC-15（含真仓库用例与 `[git]` 标签守卫）
- [x] #78 验收规格已挂到 issue（2026-09-30 已回帖），实现后转 VF-01 ~ VF-09
- [ ] CI 存在 build + test job，且对 develop 的 push/PR 生效
- [ ] 无一个用例依赖真实 LLM 或外网
- [ ] 评分卡复审分数同步（评分卡 HTML 仍为 68.5；**验证闭环维度依据已更正为 55，总分应为 69.9**，需同步到 `docs/agent-harness-scorecard.html`；注意这是记账更正，非能力提升）
