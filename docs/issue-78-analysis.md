# Issue #78 关联分析摘要

> 分析对象：Issue #78 × `p0-test-plan.md` × `agent-harness-assessment.md`
> 分析时间：2026-09-30 · 代码基线：develop `b2d41ca`
> 方法：`gh` 读取 issue 全文 + 评论；静态代码核实；与两份文档交叉比对

---

## 0. 前置：三处必须先说的事实校正

在展开分析前，有三件事会直接影响结论的可信度，必须先摊开：

| # | 事实 | 影响 |
|---|---|---|
| **A** | 两份文档**不在本仓库，也从未提交 git**。`git ls-files` 报 `did not match any file(s) known to git`，状态为 `??`（未跟踪），实体只存在于另一 worktree：`/d/develop/Workspace/workx/docs/` | 它们是**单机可见的本地稿**：不入任何 PR、团队看不到、无备份、换机即失。后续所有依赖它们的协作结论（含评审、排期）目前都不成立 |
| **B** | Issue #78 有 **0 条评论**（`gh issue view 78 --comments` 与 `/issues/78/comments` 均返回空，两份文档sequence一致） | 「讨论内容」为空是客观事实，不是读取失败。因此下文分析全部基于 **issue 正文**，无第三方补充意见 |
| **C** | 两份文档对 #78 的核心判定「**代码未实现 / 零实现**」**与代码实际不符** | 这是本次分析最重要的发现，详见 §3。它同时动摇了测试计划的一整节和评估报告的评分依据 |

---

## 1. Issue #78 核心要点

**元信息**：`ygsheep`（牧羊人）· 创建 2026-09-29 · 标签 `P0` + `enhancement` · 状态 **OPEN** · 评论 **0**

| 维度 | 内容 |
|---|---|
| **问题陈述** | Agent 缺少**强制验证闭环**：模型写完代码后倾向「自己读一遍、觉得没问题、就停」，不对照任务原文验证，也没有机制在退出前拦一道 |
| **举证（3 条）** | ① 全仓无 `PreCompletionChecklist` 类 hook/middleware；`hook_event.h` 的 8 种事件无「退出前验证」节点<br>② `skill/bundled/verify/SKILL.md` 只是软性提示，模型可完全忽略<br>③ 无自动跑测试/编译/lint 的门禁，`react_loop.cpp` 达 `max_iterations` 或模型停止即结束 |
| **影响** | 交付质量依赖模型自觉，「改完不跑测试」是最常见失败模式；**没有可 hill-climb 的信号**——agent 只能对照「自己的代码」，无法对照「任务要求」自我纠错 |
| **收益论据** | 业界公认单项收益最大的 harness 改动。LangChain 在 deepagents-cli 补此项（配合环境上下文注入），Terminal-Bench 2.0 **52.8% → 66.5%**，模型一行没改 |
| **建议（4 条）** | ① 新增 `PreCompletion` hook 事件**或复用 `Stop` 事件**，注入强制提示（回原文逐条核对 / 跑测试构建 / 与任务要求而非自己的代码比对）<br>② 复用 `src/agent/hook/` 已有 8 事件派发机制，成本主要在 prompt 与触发时机<br>③ 系统提示词引导四阶段：规划发现 → 构建（带可验证性写，并补测试）→ 验证 → 修复<br>④ 可选强化：连续 N 轮未执行过任何验证类命令时自动提醒 |
| **验收标准** | 构造 10 个「实现某函数 + 有明确验收条件」的任务，统计 agent 在结束前**实际执行过测试/构建命令**的比例，目标 **≥ 90%**（当前接近 0） |
| **关联** | 依赖 `src/agent/hook/`（已完成）；关联 `#55`（预算），可与「剩余预算不足时强制切到验证阶段」联动 |

---

## 2. 代码核实：issue 的四条主张逐条比对

两份文档都明示「本仓库的 issue 描述会过时，动手前必须回代码核实」，因此逐条验证：

| Issue 主张 | 核实结果 | 证据 |
|---|---|---|
| ① hook 只有 8 种事件、无退出前验证节点 | ✅ **属实**（事件枚举确为 8 个，无 `PreCompletion`） | `src/agent/hook/hook_event.h:25-33` |
| ① 建议「复用 `Stop` 事件」成本低 | ⚠️ **部分属实，但有关键限制**（见下） | `react_loop.cpp:1201-1227` |
| ② `verify/SKILL.md` 只是软性提示 | ✅ **属实**（1378 字节，纯 Markdown 指引，无任何强制执行路径） | `src/agent/skill/bundled/verify/SKILL.md` |
| ③ 无自动跑测试/编译/lint 的门禁 | 🔴 **不成立**（已有门禁原语，只是默认未启用） | 见 §3 |
| ③ `react_loop.cpp` 达 `max_iterations` 即结束 | ✅ **属实**（默认 40，非文档所写的 25） | `react_loop.h:189` |

### 2.1 一个 issue 未预料到的架构限制（重要）

`Stop` 事件**确实已存在且已接线**（`react_loop.cpp:1215` 派发），并且 `HookResult` 已支持 `blockingError` —— 表面看「复用 `Stop`」是条捷径。但仔细阅读派发点会发现：

- `Stop` 的派发位置在循环**收尾之后**（`:1201-1227`），此时本轮已结束；
- `blockingError` 的处理只是把 **最终答案覆写成错误文本**（`:1218-1224`），并置 `was_error = true`；
- **它不会重新进入循环**，`preventContinuation` 分支（`:1225-1227`）也只是打日志。

**结论：单靠 `Stop` 只能做到「注入一句警告」，做不到 issue 要求的核心语义「验证 → 失败 → 回灌 → 修复 → 重验」。** 真要实现闭环，必须在 loop 内部新增退出前的验证判定点（或改用外层 `GoalGuardedAgent` 式的包壳），而不是在 `Stop` 里做。这一点 issue 的建议 ① 和实现成本估算都没有覆盖到。

---

## 3. 重大发现：被两份文档共同遗漏的验证基础设施

代码里存在一套 `#31 / #32`（里程碑 0.6.x）**目标导向验证栈**，两份文档均未提及：

| 组件 | 能力 | 位置 |
|---|---|---|
| `AgentGoal` | 目标类型 `TestsPass` / `BuildClean` / `LintZero` / `FileExists` / `CustomScript` | `goal_verdict.h:27-60` |
| `GoalStatus` | `Achieved` / `Pending` / `Failed` / **`NotStarted`（本轮尚未执行过任何验证）** | `goal_verdict.h:19-25` |
| `check_goal()` | 按目标类型分派验证器，返回 `Verdict{status, detail}` | `verdict.h:40` |
| `guard_command()` | 命令白名单，字符串级拒绝 shell 分隔符/操作符，防 RCE | `verdict.h:50`、`verdict.cpp:243` |
| `parse_goal()` | 解析 `agent.goal` 串：`tests_pass` / `build_clean` / `lint_zero` / `file_exists:<path>` / `cmd:<cmd>` | `verdict.cpp:422` |
| 默认命令 | `kTestCmd="ctest --output-on-failure"`、`kBuildCmd="cmake --build . --config Debug"`、`kLintCmd="echo 'no lint config'"` | `verdict.cpp:233/235/289` |
| `GoalGuardedAgent` | 外层包壳 Loop，内嵌 ReActLoop，「验到成功为止」，`max_attempts=50` | `agent_loop_adapters.cpp:33` |

**它已经可达、且已有测试**：
- 配置项接线：`chat_session.cpp:1308` → `.goal = parse_goal(goal_spec)`
- 通过 `agent.type = "goal-guarded" | "verify"` 即可启用（`app_config.cpp:129`）
- 已有用例：`tests/unit/agent/core/test_agent_core.cpp:95-111`（`parse_goal` 各分支），标签 `[agent][goal][verify]`

### 3.1 那为什么仍然算「没做」？

因为这套能力**不是默认路径**：

- `grep check_goal|AgentGoal|has_checker src/agent/core/react_loop.cpp` → **零命中**；
- `react_loop.h:28` 只包含 `goal_verdict.h` 是为了用 `GoalStatus` 枚举（字段在 `:136`），从未调用 `check_goal()`；
- 默认 `agent.type` 为空 → 走纯 ReAct → **完全绕过验证**。

**准确表述应是**：
> 验证闭环的**机器 check 能力已完成大半**（验证原语、命令白名单、配置解析、外层包壳都已就位），缺的是 **① 接入默认 ReAct 路径 ② 默认开启而非手工 opt-in ③ 与 stop budget 联动**。
> 「零实现」不成立；「默认路径上为零」成立。

这个区分不是抠字眼——它把 #78 的实现性质从「**从零建验证器**」改成了「**接线 + 改默认值**」，工作量和风险都显著下降。

---

## 4. 对 `p0-test-plan.md` 的影响

### 4.1 需要修正的现有条目

| 位置 | 现状 | 修正建议 |
|---|---|---|
| §0.1 表格 `#78` 行 | 「GitHub 状态 OPEN / develop 合并提交 ❌无 / **现有测试：无** / 可开工 ❌需先实现」 | 「现有测试：**已存在**（`test_agent_core.cpp` 的 parse_goal / verdict 用例）；可开工：✅，但目标是**接线**，不是新建」 |
| §1.2 小节 E 标题 | 「#78 验证闭环（未实现 → 只出验收规格）」 | 改为「（能力已存在，**默认路径未接线** → 现有组件可立即开测）」 |
| §3.1 阶段 5 | 「VF-01~VF-03，前置 ❌ #78 需先实现」 | 前置改为「✅ 部分具备」，VF-03 现在就能写 |
| 附录 checklist 第 5 项 | 「#78 验收规格已挂到 issue，实现后转 VF-01~VF-03」未勾选 | ⚠️ **确实未完成且无人发现**：issue #78 **0 条评论**，VF-01~03 从未回帖到 issue。这是当前唯一一项「零成本但一直没做」的动作 |
| §2.3 标签规范 | 要求标签必含 `[#issue号]`；测试颗粒支持 `ctest -L "#78"` | 现有 goal/verdict 用例标签为 `[agent][goal][verify]`，**缺 `[#78]`** → 违反既定规范，且无法单 issue 回归 |

### 4.2 VF-01 ~ VF-03 的对应与新建议

原三条规格与新发现的代码之间的对应关系：

| 现有规格 | 对应到的既有实现 | 结论 |
|---|---|---|
| **VF-01** 未通过验证不得进入 FinalAnswer | `check_goal()` 已能返回 `Achieved/Pending/Failed`，但**无人调用它来决定是否 emit final_answer** | 规格仍有效，且现在**可以立即写失败用例**（注入脚本化模型直接吐 final_answer，断言验证层拦截） |
| **VF-02** 验证失败→回灌 LLM 重试；达上限降级为「带警告终止」 | `GoalGuardedAgent` 已有 `max_attempts=50` 的重试语义 | 建议把「达上限降级」显式补进 VF-02 断言——这是 §2.1 指出的 `Stop` 方案做不到的部分 |
| **VF-03** 项目无可用验证命令→跳过而非失败 | 已被 `kLintCmd = "echo 'no lint config'"` **部分实现**（lint 场景硬编码跳过）；另有 `has_checker()` 可查是否支持 | 需澄清：现行行为是「硬编码默认命令」，而 VF-03 要的是「**探测项目是否真的有命令**」。两者语义不同，不能直接宣称已覆盖 |

**建议新增用例**：

| ID | 类别 | 用例 | 为什么现在要加 |
|---|---|---|---|
| **VF-04** | P1 | **默认路径接入**：脚本化模型全程不调工具直接给 final_answer，断言退出前发生了验证判定（`check_goal` 被调用或注入了验证提示） | 这是 #78 的真缺口；也是唯一能证明「闭环真的关上了」的用例 |
| **VF-05** | P2 | `Stop` hook 的 `blockingError` **不得吞掉** final_answer（`react_loop.cpp:1218-1224` 的覆写行为） | 该覆写可能是缺陷；若 #78 走 Stop 路线会直接踩到 |
| **VF-06** | **P1·安全** | `guard_command` 必须拦住 `goal.command` 注入：`;` `&&` `\|` `$()` 反引号、换行 | #78 一旦落地会**自动执行命令**，这是安全前置。不测等于给自动化执行开后门 |
| **VF-07** | P2 | 「无可用验证命令」的探测行为：`custom_script` 类型 + 项目无 CMake/Makefile → 跳过而非失败 | 补 VF-03 与现行 `kLintCmd` 之间的语义差 |
| **VF-08** | P4 | headless(#77) × 验证闭环：验证执行时**不得阻塞 stdin**（无人值守场景） | 跨模块耦合，与 §1.2 的 HL-12 同类。headless 是评测入口，这里挂了等于评测全挂 |
| **VF-09** | P2 | 达 `max_iterations`（`at_limit`）时是否仍能完成一轮验证 | 直接呼应 issue 关联的 `#55` 预算联动建议 |

### 4.3 连带影响

- **§0.3 基建第 3 项 `scripted_model.h`**（原标「可选」）：VF-02 / VF-04 都依赖「第 N 轮返回什么」的多轮脚本化，**建议从「可选」提升为 #78 的必需项**，否则 VF-02 的「回灌重试」写不出来。
- **§0.2「CI 完全没有 build/test job」**：这仍是全局最大的结构性问题。即便 #78 的用例写完，不接 CI 就不会被执行——而 `p0-test-plan.md` §3.2 的 CI 接入排在阶段 6。建议至少把 §3.2 提前到与阶段 1 并行，否则 #78 的 VF 用例同样沦为「写了没人跑」。<br>→ ✅ **2026-10-01 更新**：该建议已被采纳，**阶段 6 已落地** `.github/workflows/build-test.yml`（ubuntu + `ctest -LE slow`）。<br>→ ✅ **2026-10-02 再更新**：门禁已删除 `continue-on-error: true` **转为硬门禁**，首轮暴露的平台缺陷 #104 / #105 / #107 / #108 / #109 全部关闭，实测 `ctest -LE slow` **1363 用例 100% 绿**。详见 `docs/p0-test-plan.md` §3.2 与 `docs/agent-harness-scorecard.html`「六次更新」。

---

## 5. 对 `agent-harness-assessment.md` 的影响

### 5.1 §2.2 P0-2 条目需要改写

> 原文：「P0-2 无验证闭环门禁 · ❌ **未动工**（#78 仍 OPEN）· `git grep -i "precompletion\|verification_gate" -- src` **零命中**」

两处问题：

1. **取证方法有缺陷**：grep 的是 **LangChain 自己的标识符名**（`precompletion` / `verification_gate`）。本仓库即便有等价能力，也完全可能不叫这两个名字——**事实正是如此**（叫 `check_goal` / `GoalGuardedAgent` / `Verdict`）。这个零命中不能作为「零实现」的证据。
2. **结论偏重**：应改为「**能力已存在但默认未启用、未接入默认 ReAct 路径**」。欠缺程度仍在，但性质从 0% 变为约 60%~70% 已具备、仅接线未做。

### 5.2 §2 评分卡「验证闭环 48 分」的依据动摇

> 原文依据：「**#78 未做**（零实现）；仅 #81 的收尾改动清单让「改了什么」可审计，故小幅 +8」

既然「零实现」不成立，48 分需要重新定锚。粗略拆分该维度：

| 子项 | 现状 | 大致水平 |
|---|---|---|
| 验证**原语**可用性（test/build/lint/file/cmd 判定） | ✅ 已有 | 中高 |
| **命令安全**守卫 `guard_command` | ✅ 已有 | 中高 |
| 目标声明与配置解析 `parse_goal` | ✅ 已有 | 中 |
| **默认路径是否真的执行验证** | ❌ 无 | **零** |
| **失败是否触发修复重试闭环** | ⚠️ 仅在 `goal-guarded` 模式、且非默认 | 低 |

建议将 48 上调至 **55 左右**，并明确标注「+7 来自已存在但被漏记的原语，而非实际行为改善」。**注意：这一次分数上调不代表 harness 变强了**——它自始至终就在那里，只是评估时没看见。这一点最好在文档里明说，否则会造成「分数涨了=变好了」的误读。

### 5.3 §0 与 §4 的收益预期需要降温

> 原文：「仍是唯一能把分数推到 74+ 的单项」、「+13.7 分来源」

两点修正：

- LangChain 的 52.8% → 66.5% 来自「**验证闭环 + 环境上下文注入**」的**组合**（后者对应本仓未做的 P1-1），且关键在于让验证**始终生效**。Workx 的缺口比当年的 deepagents-cli 窄得多（跑执行的机器已经有了，只是没默认开），因此**预期收益应显著低于 +13.7**，不宜继续按该数字做排期承诺。
- §3 参考分数线表（「补完 P0 → 45–58%」）把五个 P0 打包估计，其中 #78 实际未落地，该区间标签应改为「补完 P0（P0-2 除外）」，避免和 §4 的 ROI 排序自相矛盾。

### 5.4 与 #77 headless 的交互被双向遗漏

- 评估报告 §2「最长板」清单与差距清单都未讨论：**headless 是评测入口，验证闭环一旦默认开启，会在无人值守场景下执行构建/测试命令**。这既是 VF-08 的由来，也意味着 #77 与 #78 的落地顺序存在耦合：先有 headless 才能跑评测，但先有验证闭环才值得跑评测。
- 建议 §4 ROI 图中在 `P0-2` 上标注「**与 P0-1 headless 强耦合，建议相邻迭代落地**」。

---

## 6. 建议的后续行动项

按 ROI 排序（成本 vs 收益）：

| 优先级 | 行动项 | 成本 | 说明 |
|---|---|---|---|
| **P0** | 把 VF-01~VF-03 验收规格**回帖到 issue #78** | ~10 分钟 | 测试计划附录已列为待办却一直未做，issue 至今 0 评论。零成本，且能让规格可被团队评审 |
| **P0** | **修正两份文档中「#78 零实现」的判定**（测试计划 §0.1/§1.2E/§3.1；评估报告 P0-2 / 评分依据 / §3 分数线） | ~1 小时 | 错误前提正在污染评分、排期和 ROI 结论，越晚改代价越大 |
| **P0** | **把两份文档 `git add` 提交**，或至少确认保留策略 | ~10 分钟 | 目前未跟踪、单机可见、无备份。这是协作风险，不是文档质量问题 |
| **P1** | 给现有 goal/verdict 用例**补 `[#78]` 标签**，使 `ctest -L "#78"` 可用 | ~30 分钟 | 测试计划 §2.3 自己规定的标签规范，当前不合规 |
| **P1** | 新增 **VF-06（guard_command 注入防护）** | ~1 小时 | #78 一旦自动执行命令，这是唯一的安全前置，应当先于功能本体落地 |
| **P1** | 新增 **VF-04（默认路径确实执行验证）** | ~2 小时 | 唯一能证明闭环真的关上的用例；依赖 `scripted_model.h`，建议同步把 §0.3 第 3 项从「可选」提升为必需 |
| **P1** | 修正 **VF-03 语义**：区分「硬编码默认命令」与「探测项目是否真有验证命令」 | ~2 小时 | 若不澄清，会误以为该条已被 `kLintCmd` 覆盖 |
| **P2** | 在评估报告 §2.2 / §4 补充 **#77 × #78 耦合说明** | ~30 分钟 | 评测量要先有 headless，但要先有验证闭环才值得跑 |
| **P2** | 新增 **VF-05 / VF-08 / VF-09** | ~3 小时 | Stop 覆写行为、headless 不阻塞 stdin、at_limit 联动 #55 |
| **P2** | **将 §3.2 CI 接入提前**至与阶段 1 并行 | ~半天 | 全局性问题：CI 现在一个测试都不跑，#78 的用例写完也不会被执行 |
| **P3** | 重新评估 §4 ROI 排序中 P0-2 的收益预期 | ~1 小时 | 从「借鉴 +13.7」改为「基于本机缺口宽度的保守估计」 |

---

## 7. 证据索引

| 结论 | 证据位置 |
|---|---|
| hook 仅 8 事件、无 PreCompletion | `src/agent/hook/hook_event.h:25-33` |
| `Stop` 已派发但位于收尾后、不可重入循环 | `src/agent/core/react_loop.cpp:1201-1227` |
| `Stop` 的 blockingError 会覆写 final_answer | `src/agent/core/react_loop.cpp:1218-1224` |
| `max_iterations` 默认 40 | `src/agent/core/react_loop.h:189` |
| 主 ReAct 路径未调用任何验证（零命中） | `grep check_goal\|AgentGoal\|has_checker src/agent/core/react_loop.cpp` |
| 主 loop 仅用 GoalStatus 枚举 | `src/agent/core/react_loop.h:28`、`:136` |
| 目标定义与 GoalStatus | `src/agent/core/goal_verdict.h:19-25`、`:27-60` |
| 验证执行与命令白名单 | `src/agent/core/verdict.h:40`、`:43`、`:50` |
| 默认验证命令 | `src/agent/core/verdict.cpp:233`、`:235`、`:289` |
| 目标串解析 | `src/agent/core/verdict.cpp:422` |
| 配置接线 | `src/agent/core/chat_session.cpp:1308` |
| `agent.type` 启用方式 | `src/agent/config/app_config.cpp:129` |
| GoalGuardedAgent 实例化 | `src/agent/core/agent_loop_adapters.cpp:33` |
| 已有 goal/verdict 用例 | `tests/unit/agent/core/test_agent_core.cpp:95-111` |
| verify skill 为纯软性提示 | `src/agent/skill/bundled/verify/SKILL.md`（1378 字节，无强制路径） |
| 两份文档未纳入版本控制 | `git ls-files --error-unmatch` → 未匹配；`git status --short` → `??` |
| issue #78 无评论 | `gh issue view 78 --comments` 与 `/issues/78/comments` 均为空 |

---

## 8. 一句话总结

Issue #78 的诊断（缺强制验证闭环）是对的、优先级 P0 也是对的，但**「从零实现」的前提是错的**：`#31/#32` 留下的目标验证栈（原语、命令白名单、配置解析、外层包壳）已经基本就绪，真正缺的是「接入默认 ReAct 路径 + 默认开启」。两份文档因只 grep 了 LangChain 的英文标识符而漏记了这套能力，导致测试计划把 #78 整体判为不可开工、评估报告给出站不住脚的「零实现 / 48 分」。**最先该做的三件事是：把 VF 规格回帖到 issue（至今 0 评论）、修正两文档的判定前提、把两份未跟踪的文档提交进版本控制。**
