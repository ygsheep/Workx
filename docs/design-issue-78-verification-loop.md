# 设计：Issue #78 强制验证闭环（PreCompletion 门禁）

> 代码基线：develop `b2d41ca` · 编写日期：2026-09-30
> 前置阅读：`docs/issue-78-analysis.md`（推翻了「#78 零实现」的判定，本方案建立在其核实结论之上）
> 状态：**待评审**

---

## 0. 结论先行

Issue #78 描述的症状（agent 写完自己看一眼就停）完全成立，但**不必从零实现**。`#31/#32` 已留下可用的验证原语，
缺的是把它们接进**默认执行路径**。本方案推荐：

> **方案 A**：把「验证 → 失败回灌 → 重验」的判定从 `GoalGuardedAgent`（目前 opt-in）下沉到 `ReActLoop` 主循环内部，
> 复用现成的 `check_goal()` / `AgentGoal` / `guard_command()`，不新建验证逻辑。

预估工作量 **2–3 人日**（含用例），而非原 issue 隐含的「新建一个 hook 事件 + 全套验证器」。

---

## 1. 已核实的事实（方案的地基）

### 1.1 现成资产

| 资产 | 位置 | 状态 |
|---|---|---|
| `AgentGoal` 目标类型（TestsPass / BuildClean / LintZero / FileExists / CustomScript） | `goal_verdict.h:27-60` | ✅ 可用 |
| `GoalStatus`（`Unknown`/`Pending`/`Achieved`/`Failed`/`NotStarted`） | `goal_verdict.h:19-25` | ✅ 可用 |
| `check_goal()` 按类型分派验证器 | `verdict.h:40` | ✅ 可用 |
| `guard_command()` 命令白名单（字符串级拒绝 shell 分隔符，防 RCE） | `verdict.h:50`、`verdict.cpp:243` | ✅ 可用，且是安全前置 |
| `parse_goal()` 解析配置串 | `verdict.cpp:422` | ✅ 可用 |
| 默认命令 `ctest --output-on-failure` / `cmake --build .` / `echo 'no lint config'` | `verdict.cpp:233/235/289` | ✅ 可用（但需见 §5.2 成本问题） |
| `GoalGuardedAgent`：验→回灌→重验的**完整闭环** | `agent_loop_adapters.cpp:33` | ✅ 已实现，但 opt-in |

`GoalGuardedAgent` 的循环语义（每轮 ReAct 后跑 `check_goal`，未达成则注入「继续」再走一轮，
直到达成或 `max_attempts=50`）**正是 #78 要的东西**。所以本方案本质上是：
把这个已验证过的语义，从「外层包壳 + 手工开启」搬进「主循环 + 默认开启」。

### 1.2 三处决定方案选型的约束

1. **`Stop` hook 撑不起闭环**（已在 `issue-78-analysis.md` 论证）。派发点在 `react_loop.cpp:1199-1227`，
   位于循环**收尾之后**；`blockingError` 只覆写 `final_answer`（`:1218-1224`），**不会重新进入循环**。
   → 排除「用 Stop 注入提示」这条捷径。

2. **headless 绕过 `chat_session` 的目标接线**。`headless.cpp:143-150` 的 `build_loop()` **直接构造 `ReActLoop`**
   （`ReActLoop::Config loop_config` 默认构造），既不经过 `chat_session.cpp:1308` 的 `.goal = parse_goal(...)`，
   也不经过 `GoalGuardedAgent`。
   → **这一条直接否决「把 GoalGuardedAgent 设为默认」的方案**：headless 是评测入口（#77），
   若验证闭环走包壳路线，评测链路上就永远为零验证，恰好是 #78 最不能被绕过的场景。

3. **默认 `agent.type` 为空 → 纯 ReAct**（`app_config.cpp:129`），`react_loop.cpp` 对 `check_goal` 零命中。

---

## 2. 方案对比

| | 方案 A：下沉到 ReActLoop（推荐） | 方案 B：GoalGuardedAgent 设为默认 | 方案 C：Stop hook 注入 |
|---|---|---|---|
| 覆盖 interactive | ✅ | ✅ | ❌（不可重入） |
| 覆盖 **headless / 评测入口** | ✅ | ❌ 需额外改 headless | ❌ |
| 复用既有代码 | ✅ `check_goal` 等原语 | ✅ 整个包壳 | ❌ |
| 改动面 | 中：Config + 主循环一处 + 两处接线 | 小，但**必须同步改 headless** | 小 |
| 语义风险 | 低（仍在单循环内） | **高**：包壳把每轮拆成独立 `ReActLoop(max_iterations=1)`，压缩/缓存/轮次语义全变 | — |
| 结论 | **推荐** | 备选 | 已否决 |

---

## 3. 方案 A 详细设计

### 3.1 插入点

`ReActLoop` 主循环是 `while (!graceful_stop && budget > 0)`（`react_loop.cpp:648`）。
模型给出终局答复的判定在 `:746`：

```cpp
if (thought.tool_uses.empty()) {          // :746  模型不再调工具 → 准备收尾
    messages.push_back(ChatMessage::assistant(thought.content));
    result.final_answer = thought.content;   // :755
    /* ...记录 FinalAnswer 步骤 :759-771... */
    break;                                    // :773  ← 门禁插在这里之前
}
```

**在 `:773` 的 `break` 之前插入门禁**，是唯一能同时满足「覆盖全部退出路径」和「可继续迭代」的位置。

### 3.2 逻辑（伪代码）

```cpp
// === #78：FinalAnswer 前验证门禁 ===
bool gate_open = m_config.verify_before_finish
              && m_config.goal.has_goal()
              && has_checker(m_config.goal.type);          // VF-03：无可用验证器 → 直接放行

if (gate_open) {
    Verdict v = check_goal(m_config.goal, m_cwd);

    if (v.status == GoalStatus::Achieved) {
        result.goal_status = GoalStatus::Achieved;          // 通过
    } else if (++verify_attempts < m_config.verify_max_attempts) {
        messages.push_back(ChatMessage::user(build_feedback(v)));  // 回灌
        ++iteration;                                        // ⚠️ 必须手动推进，见 §5.1
        --budget;
        continue;                                           // 继续迭代 → 形成闭环
    } else {
        result.final_answer += degrade_warning(v);          // VF-02：降级为带警告终止
        result.goal_status = GoalStatus::Failed;
    }
}
break;
```

`build_feedback(v)` 注入的内容对齐 LangChain 的 `PreCompletionChecklist` 口径：

> 验证未通过（`<detail>`）。回到任务原文逐条核对，**运行项目的构建/测试命令并读完整输出**，
> 与任务要求（而不是与你自己的代码）比对，不一致就修。修完再次验证前不要声明完成。

### 3.3 配置扩展（`react_loop.h:186-204` 的 `Config`）

```cpp
AgentGoal goal;                        // 复用现成结构体 + parse_goal()，None = 不启用
bool verify_before_finish = false;     // 门禁总开关
int  verify_max_attempts  = 3;         // 失败回灌重试上限（远小于 GoalGuarded 的 50）
```

> 只加 3 个字段。现有的 `AgentGoal::max_attempts`（默认 50）不适合直接用：
> 那是独立包壳的预算，套进单循环会放大成本（见 §5.2），因此单列一个更小的门限。

### 3.4 接线（两处）

| 位置 | 改动 |
|---|---|
| `chat_session.cpp`（ReAct 路径） | 现状 `.goal = parse_goal(goal_spec)` 只写在 GoalGuarded 分支（`:1308`）。改为**对 ReAct 同样装配** `loop_config.goal` |
| `headless.cpp:147` `build_loop()` | 同一份配置注入 `loop_config.goal` 与开关 —— **这是评测链路能拿到验证的前提** |

配置项沿用既有 key：`agent.goal`（值域 `tests_pass` / `build_clean` / `lint_zero` / `file_exists:<path>` / `cmd:<cmd>`）。

### 3.5 可观测性

- 新增 step 类型还是复用 `FinalAnswer` 步骤：**待定**。新增 `Verification` 步骤类型会影响 TUI 渲染，
  MVP 建议先只在日志与结果字段里体现，不碰 TUI。
- `result.goal_status` 初始化为 `Unknown`（`react_loop.h:136`），目前**从未被赋值**，恒如此。
- **headless 的 json 输出已在写入该字段**（`headless.cpp:74`：`{"goal_status", static_cast<int>(r.goal_status)}`）。
  → 落地后被填成 `Achieved` / `Failed`，验证结果**天然能对评测 harness 暴露，无需改动输出协议**。
  这是本方案捡到的一个便宜：端到端可观测性几乎零成本就已经铺好了。

---

## 4. 落地阶段（渐进开启）

一次性把默认行为改成「强制验证」风险过高（会改变所有现网交互的成本与时延），建议分四阶段：

| 阶段 | 内容 | 默认开关 | 退出条件 |
|---|---|---|---|
| **P1 基建** | Config 字段、主循环门禁、`build_feedback`、两处接线 | `verify_before_finish = false` | VF-01/02/03/04/05/06 全绿 |
| **P2 headless 先行** | headless 默认开启门禁 | headless 内 true | VF-08 通过；抽样观察成本增幅 |
| **P3 interactive** | 交互式默认开启 | true | 观察主线程阻塞体感；VF-09 通过 |
| **P4 命令自适应** | 检测项目真实构建/测试命令，替代硬编码默认命令 | 自动 | 与 P1-1（环境上下文注入）合并做 |

> **P2 放在 P3 之前**，因为 headless 是评测入口：先让能测的场景有门禁，才能拿到 #78 的真实收益数据，
> 而不是先在交互形态上承担体验风险。

### 4.1 落地进度（截至 2026-09-30）

| 阶段 | 状态 | 落地内容 | 用例 |
|---|---|---|---|
| **P1 基建** | ✅ 已完成 | `Config` 加 `goal` / `verify_before_finish` / `verify_max_attempts`；`apply_verification_gate()` 统一装配；`run_verification_gate()` 插入 `react_loop.cpp:841-855`（FinalAnswer 落地前）；`query_engine.cpp` 接线（`enabled_by_default=false`） | VF-01/02/03/04/05/06 全绿 |
| **P2 headless 先行** | ✅ 代码已落地 | `headless.cpp:149-153` `build_loop()` 以 `enabled_by_default=true` 接线 | VF-07/08/09 —— **VF-08 待补** |
| **P3 interactive** | ⏸ 未开始 | 交互式默认开启 | 需观察主线程阻塞体感 |
| **P4 命令自适应** | ⏸ 未开始 | 探测项目真实构建/测试命令 | 与 P1-1 合并做 |

**VF-08（headless 下验证执行不阻塞 stdin）为何仍未落地**：它与 HL-01~HL-12 同属
`tests/unit/agent/headless/test_headless.cpp`，而后者依赖 #77 阶段 0 ② 的 headless 集成基建
（当前构建产物只有 `agent_unit_tests.exe`，尚无 headless 可执行目标，也无 stdin 驱动测试）。
属性本身在**构造上成立**：headless 先从 stdin 读完任务文本（`headless.h:25-26`）再调 `run()`，
验证发生在 `run()` 内部，此时 stdin 已消费完毕。故 VF-08 应随 #77 的 HL 用例一同落地，
不宜在此处用弱断言凑数。

**与 §7 已知缺口的关系**：内部评审器的两条退出路径（`:806` wrap_up、`:1149` at_limit）
仍**不经门禁**——它们是 P1 之后才处理的缺口，尚未开启任何开关时会走到，开启后依然绕过。
这是本阶段刻意留下的边界，需在 P3 之前补齐或显式接受。

### 4.2 ⚠️ 阻断验收的既有缺陷：ctest 无法选中中文名用例

验收标准「`ctest -L "issue78"` 可单独跑通」**在本机原本不可能成立**，原因是一条与 #78 无关的既有缺陷：

**现象**：`ctest -L "issue78"` 13 条全 Failed，日志为 `No test cases matched "VF-03: ..."`；
但直跑 `agent_unit_tests.exe "[issue78]"` 同样 13 条全绿。

**根因（字节级取证）**：

| 环节 | 字节 | 编码 |
|---|---|---|
| 二进制里注册的测试名 | `e6 97 a0 e5 8f af …` | **UTF-8**（3 字节/汉字） |
| 进程实际收到的 argv 过滤串 | `ce de bf c9 …` | **GBK/CP936**（2 字节/汉字） |

两者字节不通约 → Catch2 匹配失败。链路是：ctest 把测试名当**过滤参数**传给测试进程
（`CatchAddTests.cmake` 的 `add_test(NAME ... COMMAND exe "<测试名>")`），而该进程走的是
`main(int, char** argv)` —— argv 由 CRT 从 **ANSI 代码页（本机 936）** 解码；测试名却是按
UTF-8（项目全局 `/utf-8`）编进二进制的。**只要测试名含非 ASCII 字符，就无法被按名选中。**

> ⚠️ **先走了弯路，记录以免后人重蹈**：`catch_main.cpp:24` 的 `wmain` 分支判定是
> `defined(_UNICODE)`，但**在自己的目标上加 `_UNICODE` 无效** —— 该判定位于 vcpkg
> **预编译**库 `Catch2WithMain` 内部，加宏无法重编那个 TU。实测 argv 仍是 GBK。

**这是既有缺陷，不是 #78 引入**：对照组实验显示，纯 ASCII 名用例（`user_invocable false is honored`）
在 ctest 下 **Passed**，而**任何**中文名用例（含本方案未触碰的 `background: cancel() 定向取消已分发任务（P1-1）`）
一律 Failed。历史上 CI 从不跑测试（见 `p0-test-plan.md` §3.2），所以一直没暴露。

**本仓已有的半套补丁**：`cmake/CatchAddTests_utf8.cmake` 只修了**发现**阶段
（发现前 `chcp 65001`，否则 CMake 解析中文名 JSON 会失败）。**执行**阶段未修 —— 而验收标准
`ctest -L "issue78"` 恰恰依赖执行阶段。

**对策**：在 `tests/test_main.cpp` 自定义 `wmain`，用 `GetCommandLineW` 拿未转码的 UTF-16
命令行，再转 UTF-8 交给 `Catch::Session()`，两侧编码即一致。**不再依赖 `Catch2WithMain` 的
`main`**（静态库惰性链接，其 `main` 不会被拉入 → 无重复符号），同时自行实例化
`Catch::LeakDetector` 以保留退出期泄漏报告。无 CMake 改动。

**对 P0 测试计划的连带影响（重要）**：`p0-test-plan.md` §2.3 规定用例名用中文行为描述、§3.2/§6 又要求
CI 以 `ctest -LE slow` 作硬门禁 —— **这两条在修复前互相矛盾**：只要门禁用 ctest 逐用例跑，中文名用例就必然全红。
这条应回填到测试计划的 CI 方案里（修复落地后已不矛盾，但 CI 首次接线时要先确认此项已合入）。

---

## 5. 风险与对策

### 5.1 ⚠️ 预算记账（最可能的 bug）

主循环的 `--budget` 在**循环体末尾**（`:799` / `:871` / `:1128`）。用 `continue` 提前回到顶部会**跳过递减**，
若不手动 `--budget` / `++iteration`，会造成**无限循环**。

对策：回灌分支内显式递进（见 §3.2 伪代码），并补一条用例锁住：脚本化模型连续 N 轮只给终局答复、
从不修复 → 必须在有限步内降级退出，不得打满 CPU 空转。

### 5.2 成本爆炸

默认 `kTestCmd = "ctest --output-on-failure"` 是**全量测试**。每轮失败都重跑一次全量 ctest，
配合 40 轮预算，成本可能放大数倍。

对策（按优先级）：
1. `verify_max_attempts` 封顶 3 次 —— 成本上界确定；
2. **去抖**：自上次验证以来没有任何新工具调用，则不重复执行，直接复用上次 Verdict；
3. 允许 `agent.goal = "cmd:<命令>"` 精确指定（如只跑单个 ctest 子集）。

### 5.3 安全

验证闭环一旦默认开启，**系统将自动执行构建/测试命令**。因此 **`guard_command()` 是硬性前置**，不是可选项。
`goal.command` 必须过白名单（`verdict.cpp:243` 已实现），并补 VF-06 锁住注入向量。
**VF-06 应先于功能本体合入。**

### 5.4 与其他模块的交互

| 交互方 | 说明 |
|---|---|
| **#77 headless** | 强耦合。headless 是评测入口，也是本方案 P2 阶段首个受益方；同时验证不得阻塞 stdin（VF-08） |
| **#55 预算** | issue 原文建议联动。P4 阶段再做「剩余预算不足时强制切到验证」，MVP 不引入该耦合 |
| `#81 git checkpoint` | 已有 `react_loop.cpp:622` 捕获基线。验证失败回灌时不应重复捕获（幂等，已自保证） |
| 内部评审器 `review_enabled` | `:806` / `:1149` 两条退出路径目前**不经门禁**。MVP 只守 FinalAnswer 路径，留下缺口（见 §7） |

---

## 6. 用例映射（`docs/p0-test-plan.md` 的 VF 系列）

落地位置：`tests/unit/agent/core/test_react_loop.cpp`，脚本化模型驱动（无真 LLM）。

| ID | 类别 | 对应本设计的验证点 |
|---|---|---|
| VF-01 | P1 | 门禁未通过时不产生 FinalAnswer（或等于降级文本） |
| VF-02 | P1 | 回灌后模型修复 → 下一轮 Achieved；达 `verify_max_attempts` 后降级为带警告终止 |
| VF-03 | P2 | `goal.type == None` 或 `has_checker() == false` → 直接放行，不执行任何命令 |
| VF-04 | P1 | **默认路径接入**：headless 的 `build_loop()` 产出的 loop 也带 goal（防止 §1.2 约束 2 复发） |
| VF-05 | P2 | `Stop` 的 `blockingError` 覆写 `final_answer`（`:1218-1224`）不吞掉验证结论 |
| VF-06 | **P1·安全** | `guard_command` 拦截 `goal.command` 注入（`;` `&&` `\|` `$()` 换行） |
| VF-07 | P2 | 「无可用验证命令」的探测语义（区分硬编码默认命令 vs 真实探测） |
| VF-08 | P4 | headless 下验证执行不阻塞 stdin |
| VF-09 | P2 | 达 `max_iterations`（`at_limit`）时的验证行为 |

> 前置：需要 `scripted_model.h`（测试计划 §0.3 第 3 项），建议从「可选」提升为**必需**——VF-02 的多轮回灌写不出来。

**另外**：现有 goal/verdict 用例标签为 `[agent][goal][verify]`，缺 `[issue78]`，不满足测试计划 §2.3 自订规范，
无法 `ctest -L "issue78"` 回归。补标签应与 P1 阶段同 PR。

> **落地时改用了 `[issue78]` 而非本文原写的 `[#78]`**。Catch2 官方规定
> "All tag names beginning with non-alphanumeric characters are reserved"，且 `[#]`
> 专用于 `-# / --filenames-as-tags` 生成的文件名标签（如 `[#test_agent_core]`）——
> 用它做 issue 标签会占用保留命名空间。§8 的验收标准应以 `[issue78]` 为准。

---

## 7. 已知缺口（本方案刻意不为）

1. **内部评审器的退出路径不经门禁**（`:806` wrap_up、`:1149` at_limit）。这两条路径也会产出 final_answer，
    MVP 不拦截。若要「所有出口一律验证」，需在这两处也插入同一段判定——建议抽取成函数 `bool try_pass_gate(...)`，
    但那会让 P1 改动面扩大，故留到 P3 之后处理。
2. **不自动检测项目的构建/测试命令**。MVP 依赖配置显式声明 + 硬编码默认命令，
   「自适应探测」属 P1-1（环境上下文注入）的范围。
3. **不改 headless 退出码语义**。验证失败时 `result.was_error` 是否置位、退出码是否变化，
   会影响评测 harness 的解析，需单独决策；MVP 只在 `goal_status` 字段体现。

---

## 8. 验收标准

对齐 issue #78 原文：

> 构造 10 个「实现某函数 + 有明确验收条件」的任务，统计 agent 在结束前**实际执行过测试/构建命令**的比例，
> 目标 **≥ 90%**（当前接近 0）。

补充两条本方案特有的验收：

- [x] `ctest -L "issue78"` 可单独跑通（**13/13，100% passed**）；
      VF-01 ~ VF-07 / VF-09 全绿。此前 13 条全 Failed，根因见 §4.2（已修复）
- [ ] **VF-08 待补** —— 依赖 #77 的 headless 集成基建，见 §4.1
- [ ] headless 的 json 输出中 `goal_status` 不再是恒 `Unknown`（字段已就绪 `headless.cpp:74`；
      需在配置了 `agent.goal` 的 headless 实跑中抽样确认，属 P2 抽样项）

---

## 9. 待确认决策点（评审时拍板）

| # | 问题 | 倾向 |
|---|---|---|
| 1 | MVP 是否要覆盖内部评审器的两条退出路径（§7.1）？ | 否，留到后续 |
| 2 | 是否新增 `Verification` step 类型暴露到 TUI？ | 否，MVP 只在日志/结果字段 |
| 3 | 验证失败是否要让 headless 退出码变化？ | 否，先只用 `goal_status` |
| 4 | P2（headless 先行）能否直接跳到 P3（全面开启）？ | 否，先在能测的场景拿到收益数据 |
| 5 | `verify_max_attempts` 默认 3 是否合适？ | 待实测调优，先保守取 3 |
