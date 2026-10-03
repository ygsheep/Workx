# Workx Agent Harness 能力评估与改进路线

> 初评 2026-09-29 · **复审 2026-09-30**（develop `b2d41ca`）· **三次更新 2026-09-30 18:55**（develop `9a2de19`）· **四次更新 2026-10-01**（阶段 6：CI 接入）· **六次更新 2026-10-02**（CI 转硬门禁 + 平台缺陷清零）· **八次更新 2026-10-03**（#78 完整落地：P3/P4/VF-08，PR #116 `49d0ca4`）· 对象：`Workx v0.10.1`（`src/core` + `src/agent` + `src/tui`）
> 方法：**静态代码审计 + 业界 Harness 评测方法论对齐**。
> ⚠️ 本文的分数是**基于代码的预估分，不是跑分结果**。**P0 五项已全部完整落地**（含 **P0-2 #78 的 P1/P2/P3/P4 + VF-01~VF-11**，PR #116 `49d0ca4` 收尾，**issue 已于 2026-10-02 关闭**）；headless 入口已通，**但仍未跑过任何基准**，真实分数必须用 §3 的方案实测。**#78 的原始验收口径（行为统计：结束前执行过验证命令的比例 ≥90%）尚未实测，已并入首轮跑分采集，见 §3 Step 2b。**
> 📌 可视化评分卡：`docs/agent-harness-scorecard.html`
> 🔴 **二次更正 2026-09-30 13:20**：P0-2「零实现」的判定**不成立**，`#31/#32` 的目标验证栈已存在（详见 §2.2 P0-2）。
> 该更正使「验证闭环」维度由 48 上调至 **55**，加权总分由 68.5 → **69.9**。
> ⚠️ **那是记账更正，不是能力提升**——能力自始至终就在代码里，只是初评与复审都没看见，误记为 0。
> ✅ **三次更新 2026-09-30 18:39**：#78 的 P1 / P2 已落地（`9a2de19` / PR #98）——主循环自带 FinalAnswer 前验证门禁，headless 默认开启，13 条用例全绿。
> 「验证闭环」维度 **55 → 65**，加权总分 **69.9 → 71.9**。⚠️ **这一次是真实的能力提升**（与二次更正性质不同）：默认路径开始真的执行验证了。
>
> 🛠 **四次更新 2026-10-01**：**阶段 6「CI 接入」已落地** —— 新增 `.github/workflows/build-test.yml`（ubuntu + `ctest -LE slow`），补齐仓库**从未在 CI 跑过任何测试**的门禁缺口（`code-quality.yml` 只有 lint，`release.yml` 只 build 不跑测试）。
> 已在 WSL Ubuntu 24.04 + 全新 vcpkg 上**按真实 CI 步骤预演**：4 个测试目标编译全通，`ctest` **98% passed / 23 failed out of 1342**。
> 首轮即暴露 5 类 Linux 平台缺陷（#104 / #105 / #107 / #108 / #109），门禁先以 `continue-on-error` 观察期运行。
> ⚠️ **加权总分维持 71.9**：CI 跑的是**单元测试**而非评测基准，故「评测与迭代」维度（45 / 100）不变 —— 该维度衡量的是「有没有接 Terminal-Bench / SWE-bench」，下一步仍是 §3 Step 2（Harbor adapter + 子集跑分）。
>
> ✅ **六次更新 2026-10-02**：**CI 已由观察期转为硬门禁** —— `build-test.yml` 删除 `continue-on-error: true`（PR #111 `fcb2aad`），`Build & unit tests (ubuntu)` 实测 **pass（5m47s）**，`ctest -LE slow` **1363 用例 100% passed / 0 failed**。
> 首轮暴露的 5 类平台缺陷**已全部关闭**：#105（island IPC `close()` 补 `shutdown()` + `connect()` 仅对 ECONNREFUSED 重试）/ #104（`python_command.h` 兜底 `python3` + `SIGPIPE` 改 `SIG_IGN`）/ #107（CI 装 ripgrep）/ #109（盘符统一小写 + 按平台精确断言）/ #108（撤销 vendored ftxui 的 SYSTEM 标记，PR #115 `7147eeb`，本机 tui 恢复可编译、253 用例全过）。
> 兜底同时增强：新增 island hang canary（`-L island -LE slow --timeout 45`，让挂死 1 分钟内点名暴露）、`ctest --timeout` 180 → 90、用例数下限守卫实测 1363。
> ⚠️ **加权总分仍维持 71.9**：硬门禁拦的是**回归**，不等于接入了评测基准 ——「评测与迭代」维度不变，下一步仍是 §3 Step 2。
>
> 🔴 **七次更新 2026-10-02 18:40（真实 CI 数据复核）**：把 `build-test.yml` 的**全部历史运行**拉出来核对，纠正了六次更新的绿灯声明。
> `Build & Test` **8 次首次运行：5 绿 3 红**，3 次失败**全部命中同一个用例** —— `server: ring buffer replays missed events on reconnect`（`test_island_server.cpp:35/43/190`，连接建立失败，耗时稳定 3.00~3.11s）。
> **flaky 铁证**：失败横跨不同提交（后两次是**零代码变更的纯 docs 提交** `74de290` / `83e4b41`），且对同一 commit `83e4b41` 重跑 → **由红转绿**（island 43/43、全量 **1363/1363**，4.98s）。
> **根因**：`island_server.cpp:122-156` 的 accept 循环里 `handle_connection()` 是阻塞的，而 `transport_posix.cpp:105-115` 又在 `accept()` 成功后**立刻关闭监听端点** → **整个服务连接期间端点上没有监听者**，重连只能靠 `connect()` 的 600ms 固定重试（`kConnectAttempts 6 × 100ms`）撞运气，共享 runner 调度抖动一超即 `ECONNREFUSED`。附带缺陷：测试的 `read_until` 在 read 立即失败时不 break，空转满 3 秒 —— 这正是那个稳定耗时数字的来源，它把「连接失败」包装成了「看起来像超时」。
> → **加权总分 71.9 → 71.7**（仅「编排与可靠性」72 → 70：CI 门禁是项目**唯一**的自动化回归防线，实测 37.5% 误拦会让人对红灯脱敏）。**其余六维无新证据支撑变动**——「全量 1363 用例 100% 通过」已被重跑独立证实，说明此前的静态预估是稳的。

---

## 0. 一句话结论

Workx 的**工具层和安全层**完成度相当高（22 个工具、并发执行、fail-closed 权限、密钥脱敏、四级缓存感知压缩、MCP、Hooks、Plan mode 双防线），在自研 harness 里属于上游水平；
**验证闭环（Verification Loop）此前几乎空白，现已完整落地 P1 / P2 / P3 / P4**（主循环门禁 + headless 与交互式均默认开启 + 命令自适应探测），**非交互执行模式也已打通**（#77）；
但**仍没有任何能力评测集**——而业界 2026 年的共识恰恰是：这三项才是拉开 harness 差距的地方（LangChain 仅靠补验证闭环 + 环境上下文，Terminal-Bench 2.0 从 52.8% → 66.5%，+13.7 分，模型一行没改）。

**加权总分：72.9 / 100**（初评 61.05 → 复审 68.5 → 二次更正 69.9 → 三次更新 71.9 → 七次 71.7 → **八次更新 72.9**；七次依据真实 CI 数据微调、八次依据 #78 完整落地；详见 §2）

**复审一句话**：P0 五项**已全部完整落地**——前四项把「能不能测」这个前置问题解决了；**P0-2（#78）已完成 P1 / P2 / P3 / P4 与 VF-01~VF-11**（PR #116 `49d0ca4` 收尾并关闭 issue），验证闭环既跑在评测入口 headless 上，也跑在真实用户路径交互式上，且命令改为按项目线索自适应探测。**唯一未竟的是它原始的行为统计验收口径（≥90%），已并入首轮跑分采集（§3 Step 2b）。**
初评「补齐 P0 后约 82 分」的预估仍偏乐观，实际 **72.9** —— 因为「评测与迭代」维度的分数取决于**真的跑过分**，而这件事至今一次都没做。
**七次更新一句话**：真的把 CI 跑起来核对了 —— **1363 个单测确实全绿**（同 commit 重跑独立复现），但**硬门禁本身有 37.5% 的 flaky 误拦**，`develop` 曾因此连续两次变红。**「有真实数据」和「数据可信」是两件事**。

> 🔴 **更正**：原判「#78 零实现」应改为「**#78 的能力大部分早已存在，缺的是接线与默认开启**」。`#31/#32` 留下的
> `goal_verdict.h` / `verdict.h`（`check_goal` / `guard_command` / `parse_goal`）/ `GoalGuardedAgent` 均已实现且有测试，
> 只是 `react_loop.cpp` 从不调用 `check_goal()`，默认 `agent.type` 为空 → 纯 ReAct → 零验证。
> ✅ **现已完整落地**（P1/P2 于 2026-09-30 PR #98；**P3/P4 + VF-08 于 2026-10-02 PR #116 `49d0ca4` 收尾并关闭 issue**）：
> `run_verification_gate()` 进主循环（FinalAnswer 落地前），**headless 与交互式均默认开启**，命令按项目线索自适应探测
> （CMake 须真生成过 CTestTestfile / Cargo.toml / go.mod / package.json / Makefile / pytest），探测不到则 `unavailable` 放行而非误判失败；
> `[issue78]` **20 用例 / 105 断言全绿**。
> ⚠️ **唯一未竟**：本 issue 的原始验收口径是**行为统计**（10 个任务中结束前实际执行过测试/构建命令的比例 ≥90%），
> **尚未实测** —— 单测只能证明门禁接线正确，证不了真实任务下 agent 真的会去跑。已并入首轮跑分采集（§3 Step 2b），不达标再开针对性 issue。

---

## 1. 业界怎么给一个 Coding Agent Harness 打分

### 1.1 基准矩阵（Benchmark）

| 基准 | 测什么 | 治理方 | 备注 |
| --- | --- | --- | --- |
| **SWE-bench Verified** | 500 个人工校验 GitHub issue，产出 patch 并通过测试 | SWE-bench 团队 | 最常被引用的数；厂商爱报这个，因为最高 |
| **SWE-bench Pro** | 731 道跨文件、长上下文难题 | Scale AI | 比 Verified 普遍低 20–25 分，这个差是**结构性**的 |
| **SWE-bench Lite** | 300 题子集 | SWE-bench 团队 | 成本敏感对比首选（HarnessTax 用它） |
| **Terminal-Bench 2.0 / 2.1** | 89 道真实终端端到端任务 | Stanford + Laude Institute | **CLI agent 的权威榜**；2.1 修了 28 道题，仅"换尺子"就让 Claude Code+Opus4.6 涨 12.1 分 |
| **Aider Polyglot** | 多语言代码修改 | Aider（厂商自控） | 仅作参考 |
| **MCP Atlas** | 工具使用能力 | 第三方 | 评估工具设计质量的主战场 |
| **Frontier-Bench v0.1** | 74 题跨 7 领域（软件/ML/安全/硬件/媒体…） | Terminal-Bench + Harbor 团队 | 2026-07 发布，因 Terminal-Bench 饱和（头部挤在 74–84%）而诞生；当前 SOTA 仅 34.4% |

### 1.2 三条必须知道的方法论（否则分数没意义）

1. **Harness 差异可达 10–20 个百分点，比模型之间的差距还大。**
   Terminal-Bench 2.1 官方榜上，同一模型在原生 harness 与统一 harness（Terminus 2）下差 3.4–5.1 分，而 #1 与 #3 模型的差距也就这个量级。**报"Terminal-Bench 分数"不附版本号和 harness 名，等于没报。**

2. **Harness 更影响成本，而不是成功率。**
   UC Berkeley × Arena 的 [HarnessTax](https://harnesstax.github.io/) 研究（21 组模型×harness 组合，SWE-bench Lite + Terminal-Bench 2.0，各 30 题 × 3 次）结论：换 harness 成功率变化普遍在 ±2%（SWE-bench Lite）和 ±5%（Terminal-Bench）以内，但**成本最高差 5 倍**。同样轮数下 Claude Code 成本约为极简 harness Pi 的 2 倍、成功率只多 1.1 分——这叫 **harness tax**。
   → 评分必须画 **成本–成功率 Pareto 前沿**，只看成功率会奖励"烧钱换分"的 harness。

3. **没有跑过 eval 的 harness 就是没测过的 harness。**
   LangChain《2026 State of Agent Engineering》：57% 的组织已上生产 agent，但 **48% 不做离线评测、63% 不做在线监控**。

### 1.3 Harness 的可调旋钮（Knob）

LangChain 把优化空间收敛成三个：

| 旋钮 | 具体手段 | 实测收益 |
| --- | --- | --- |
| **System Prompt** | 引导"规划→构建→验证→修复"四阶段；强调写可测代码、覆盖边界；注入时间预算警告 | 显著 |
| **Tools** | 目录上下文注入、环境探测注入 | 显著 |
| **Middleware / Hooks** | `PreCompletionChecklist`（退出前强制验证）、`LocalContextMiddleware`（启动时映射目录结构）、**Loop detection**（检测重复文件编辑，防 doom loop）、Reasoning sandwich（规划/验证用高推理，实现用中推理） | **最大** |

Anthropic 的补充（工具设计五原则）：少而精（工具多了反而选错）、namespacing 消除歧义、返回**有意义的自然语言上下文**而非裸 ID、响应做 token 效率优化（分页/截断/verbosity 枚举）、**工具描述本身要做 prompt engineering**（Sonnet 3.5 当年靠"打磨工具描述"就刷到了 SWE-bench SOTA）。配套流程是 prototype → eval → 让 agent 自己改工具 → 复测，**必须留 held-out 集防过拟合**。

### 1.4 跑分工具链

- **[Harbor](https://github.com/harbor-framework/harbor)**：Terminal-Bench 官方 runner，支持自定义 agent adapter + 第三方数据集（SWE-bench、Aider Polyglot…），可 Daytona/Modal 并发上千环境。
  `harbor run --dataset terminal-bench@2.0 --agent <your-agent> --model <m> --n-concurrent 4`
- **推荐指标**：pass@1（3 次重复取平均 + bootstrap 95% CI）、平均 turns、token 成本、wall-clock、工具错误率、卡死/中断率，最后画 Pareto 前沿。

---

## 2. Workx 评分卡

评分口径：0–100，权重按"对最终任务成功率的影响"分配（验证闭环权重最高，因为它是最被低估也最有效的一项）。

| # | 维度 | 权重 | 初评 | 复审 | 三次 | **七次** | 加权 | 变动依据 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 工具设计与执行 | 20 | 72 | 82 | 82 | **82** | 16.4 | **#80**：`json_schema.{h,cpp}` 275 行实现，接入 `executor.h:223` 作 `validate_input` 后兜底；新工具漏写不再裸奔。CI 实测（含 `[tool][grep][search]` 5 项，修 #107 后）全绿 |
| 2 | 上下文工程 | 20 | 70 | 70 | 70 | **70** | 14.0 | 未动（#82 环境上下文注入未做） |
| 3 | **验证闭环** | 20 | 40 | 55 | 65 | **70** | 14.0 | **#78 已完整落地**（P1/P2 `9a2de19` + P3/P4/VF-08 `49d0ca4`）：门禁进主循环、headless 与**交互式均默认开启**、命令自适应探测且 `unavailable` 与失败分离 → 较七次 **+5**（交互式真实用户路径此前仍是零验证，这次才真正关上）。**不给更高档的原因**：原始行为统计验收口径（≥90%）未实测，属「机制完整但效果待证」 |
| 4 | 安全与权限 | 15 | 68 | 75 | 75 | **75** | 11.25 | **#81**：基线 commit + 人工回滚入口（只读安全网）+ #88 文件权限 0600 + #84 沙箱降级可见化 / Job Object；白名单档（#85）仍未修 |
| 5 | 编排与可靠性 | 10 | 62 | 72 | 72 | **70** | 7.0 | **#79**：`SubAgentBudget` 批量 + run 累计双护栏、atomic CAS 预约。🔴 **七次扣 2 分**：真实 CI 数据实测硬门禁 **37.5% flaky 误拦**（同 commit 重跑翻转）—— 该维度含「可靠性」，而 CI 门禁是本项目**唯一**的自动化回归防线 |
| 6 | 可观测性与成本 | 10 | 75 | 78 | 78 | **78** | 7.8 | headless json 输出带 `usage`；收尾报告含 `git_diff_summary` |
| 7 | **评测与迭代** | 5 | 15 | 45 | 45 | **45** | 2.25 | **#77**：headless 已通（`-p` + 三种输出格式 + 语义化退出码），前置解锁；CI 已真跑 1363 单测，但**无 adapter、无 eval 集、未跑过任何基准** |
| | **合计** | 100 | 61.05 | 68.5 | 71.9 | **72.9** | — | 二次更正 48→55 是记账更正；三次 55→65 是 #78 P1/P2 的真实提升；七次 71.9→71.7 是真实 CI 数据对门禁可靠性的修正；**八次 71.7→72.9 是 #78 完整落地（P3 交互式默认开启为真实用户路径补上验证）** |

### 2.1 已经是长板的部分（别动，写进卖点）

- **工具执行七步漏斗 + 并发**：同一批 tool_calls `std::async` 并行，且用 `wait_for(100ms)` 轮询保证取消即时生效。
- **权限 fail-closed**：拿不到 event_bus → 拒绝；超时 → 拒绝；用户取消 → 拒绝。这是正确方向，很多 harness 是 fail-open。
- **四级缓存感知压缩**：0.5/0.6/0.8/0.9 水位 + SoftNotice/Snip/Compact/Force/Stuck 五种动作，还专门处理了 prompt 缓存失效——这个是认真设计的。
- **密钥扫描**：写入拦截 + 输出 `[REDACTED:x]` 脱敏，审计日志也脱敏。
- **MCP + Hooks 双全**：stdio/HTTP 传输、OAuth、子 Agent 独立作用域；8 种 hook 事件。
- **1363 个分层单元测试**（`ctest -N -LE slow` 实测，2026-10-02 重跑复核；4 个测试目标 core / agent / island / tui + 测试文件 helpers 5 个），LLM 侧有 mock。CI 硬门禁实测 `ctest -LE slow` **1363/1363 通过，whole-suite 4.98s**。

### 2.2 差距清单（按严重度）

#### P0 — 阻塞级：不修就没法测、也没法谈分数

> **复审状态：5 项中 4 项已合入 develop**（2026-09-30）。下表「状态」列标记实际落地情况。

| ID | 问题 | 状态 | 落地证据 / 残留问题 |
| --- | --- | --- | --- |
| **P0-1** | 没有非交互 / headless 执行模式 | ✅ **已修**（#77，PR #90） | `src/agent/headless/headless.{h,cpp}`；CLI `-p/--print` + `--output-format`（`main.cpp:89-166`）；退出码 0/1/2 ⚠️ **零测试覆盖** |
| **P0-2** | 无验证闭环门禁 | ✅ **已完整落地并关闭**（#78，P1/P2 PR #98 + P3/P4/VF-08 PR #116；`closedAt 2026-10-02T14:54Z`） | ✅ **已接线**：`run_verification_gate()` 位于 `react_loop.cpp` 主循环（FinalAnswer 落地之前），验证未通过 → 回灌重试，达 `verify_max_attempts` 后带警告降级；**headless 与交互式均以 `enabled_by_default=true` 默认开启**；命令改为 `detect_goal_command()` **按项目线索自适应探测**，探测不到返回 `unavailable` 并放行（不把「没得跑」误判为「跑了没过」）。<br>🔴 **原判「零实现」不成立**：`git grep -i "precompletion\|verification_gate" -- src` 零命中只能证明**没用 LangChain 的命名**——本仓等价能力叫 `goal_verdict.h`（`AgentGoal` / `GoalStatus`）、`verdict.h`（`check_goal` / `guard_command` / `parse_goal`）、`GoalGuardedAgent`（`agent_loop_adapters.cpp:33`）。<br>✅ **顺带修掉 lint 假绿**：旧默认命令 `echo 'no lint config'` 恒定退出 0 = 「没有 lint 配置也永远判定通过」，现探测不到即如实放行。<br>⚠️ **附条件**：原始验收口径是**行为统计**（结束前执行过验证命令的比例 ≥90%），**未实测**，已并入首轮跑分（§3 Step 2b）。另有内部评审器两条退出路径仍不经门禁（未列入 issue 范围） |
| **P0-3** | 子 Agent 递归无上限 | ✅ **已修**（#79，PR #92） | `SubAgentBudget` 批量 + run 累计双护栏、atomic CAS；配置 `agent.sub_agent_max_batch/total` 📌 **复审修正**：递归深度其实已被结构性禁止（`agent_tool.cpp:80` `continue` 掉 `kAgentToolName`，深度恒为 1），真实缺口是横向规模 |
| **P0-4** | JSON Schema 校验器是空壳 | ✅ **已修**（#80，PR #90） | `json_schema.{h,cpp}` 275 行；接入 `executor.h:223` 作 `validate_input` 后兜底 ⚠️ ~~现有 8 条用例只测纯函数，未测「执行侧接入」与「错误回灌自纠」~~ → **更正**：执行侧接入（JS-15）已由 `test_json_schema.cpp:160` 的 `[json_schema][executor]` 覆盖（第 8 条即执行侧集成用例，此前被漏看）；真缺口仅剩「错误回灌自纠」（JS-16） |
| **P0-5** | 无 git checkpoint / 回滚 | ✅ **已修**（#81，PR #93） | `util/git_checkpoint.{h,cpp}`；`react_loop.cpp:622` 捕获基线、`chat_session.cpp:496` reset、headless 输出 `git_diff_summary`、TUI `/diff` + `/rollback --confirm`。只读安全网，不自动回滚 |

#### P1 — 高分位差：直接对应分数

| ID | 问题 | 建议 |
| --- | --- | --- |
| **P1-1** | 无环境上下文注入（LocalContext） | agent 启动时注入：目录树骨架、`python/cmake/ctest/npm` 等工具链探测结果、项目的构建/测试命令（可从 CMakePresets/CTest 推导） |
| **P1-2** | 无时间/轮次预算提示 | 剩余预算低时注入 warning，推 agent 从"改"切到"验"——模型极不擅长估计时间 |
| **P1-3** | Windows 无真实沙箱 | `sandbox_adapter.cpp:418` 走 `make_degraded(..., "none")`，而 Windows 是 README 宣称的一等平台。至少做 Job Object / AppContainer 或受限 token；否则 Bash 是裸奔 |
| **P1-4** | 命令拦截是**黑名单** | `permission_ask.cpp:124-144` 固定 pattern 列表，黑名单天然可绕过。补"白名单模式"（默认拒绝 + 用户显式放行）作为严格档 |
| **P1-5** | `AGENTS.md` 加载不到 | `memory.h:68/80` 只认 `CLAUDE.md` 和 `AGENT.md`（无 S）。本仓库根目录的 `AGENTS.md` 是**静默不生效**的，只因同时存在 `CLAUDE.md` 才没暴露。改成 `AGENTS.md / AGENT.md / CLAUDE.md / WORKX.md` 全兼容 |
| **P1-6** | Plan/PermissionMode 不随 resume 持久化 | `react_loop.h:314-316` 注释自认"只读边界静默丢失" → resume 后可能变可写 |
| **P1-7** | API Key 明文落盘且无权限加固 | `~/.workx/config.json` 的 `backend.api_key`（`app_config.h:31`），全仓无 `chmod/0600` 相关处理。补文件权限 0600 + 优先系统凭据存储 |
| **P1-8** | 无模型降级 / 无 run 预算上限 | `retry.h` 有指数退避（3 次 / 1s–60s）但 429 耗尽即失败。补：备用模型 fallback、单次 run 的 token/费用硬上限 |

#### P2 — 工程化与文档债

| ID | 问题 |
| --- | --- |
| P2-1 | 无 run trace 导出与失败模式自动聚类（`ExecutionTrace` 已删，`executor.h:98-99` 挂"后续 issue"） |
| P2-2 | Token 计数是启发式（chars/4、JSON chars/2），与真实 tokenizer 偏差大 → 压缩水位判断失真。用 provider 返回的 usage 做周期性校正 |
| P2-3 | 文档与实现漂移：`docs/full-guide.md:82-122` 仍描述**不存在的 `src/app/`**；压缩水位文档写 0.60/0.75/0.85/0.95，代码是 0.5/0.6/0.8/0.9；`max_iterations` 文档说 25，代码是 40 |
| P2-4 | 残留 `chat_session.cpp.bak` / `chat_session.h.bak` |
| P2-5 | 工具描述未做系统化的 eval 驱动优化（Anthropic 五原则）；22 个工具已接近"选择困难"阈值，考虑按意图合并 + namespacing（`mcp__xxx` 已有，内置工具可跟进） |
| P2-6 | `react_loop.cpp:1039-1040` 自注"孤儿线程不可真正 detach（残余风险记入 P3 保险丝）" |
| P2-7 | `WebSearchTool` 标注 "P0 简化版；P1 迁移到 MCP"（`factory.cpp:352`） |

---

### 2.3 真实 CI 运行数据（七次更新，2026-10-02）

> 以下全部取自 GitHub Actions 的**实际运行记录**（`gh run list` / `gh run view --log`），不是代码审计推断。

**`build-test.yml`（Build & Test，硬门禁）—— 8 次首次运行：5 绿 3 红**

| 时间 (UTC) | commit | 事件 | 结果 | 备注 |
| --- | --- | --- | --- | --- |
| 10-01 08:49 | `1e4b792` | PR #111 | ❌ | island canary 1/43 失败，3.00s |
| 10-01 09:03 | `4f1c6b6` | PR #111 | ✅ | |
| 10-01 10:04 | `2cc4abd` | PR #111 | ✅ | |
| 10-01 10:26 | `fcb2aad` | push（转硬门禁） | ✅ | island 43/43 · 全量 1363/1363 |
| 10-01 14:16 | `d68970e` | PR #115 | ✅ | |
| 10-01 14:36 | `7147eeb` | push | ✅ | |
| 10-02 08:07 | `74de290` | push（**纯 docs**） | ❌ | island canary 1/43，3.11s |
| 10-02 08:14 | `83e4b41` | push（**纯 docs**） | ❌ | island canary 1/43，3.11s |
| rerun | `83e4b41` | **同 commit 重跑** | **✅** | island 43/43 · 全量 1363/1363，4.98s |

**三条结论**

1. **测试本身是健康的**：`ctest -LE slow` 注册 **1363** 个用例，在能跑到的场合 **100% 通过**，whole-suite 实测仅 **4.98s**（`-j nproc`，Release）。4 个测试目标在干净 Linux 环境下全部可编译。
2. **门禁不可信**：唯一失败用例是 `server: ring buffer replays missed events on reconnect`，8 次里出现 3 次（**37.5%**）。失败横跨不同提交、且包含零代码变更的纯 docs 提交 —— 是 **flaky**，不是回归。同 commit 重跑即翻转，是最终确证。
   附带一个解释性事实：**Windows 侧同路径的重试窗口约 15s**（`transport_win32.cpp:80-98`，`WaitNamedPipeA(5000)` × 3），是 POSIX 那 600ms 的约 **25 倍** —— 这正是「本机开发从来没碰到过」的原因，也提示修复应让两侧语义对齐而非给 POSIX 单边加码。
3. **兜底设计是有效的**：`Run island tests (hang canary)`（`-L island -LE slow --timeout 45`）把失败在 1 分钟内点到了**具体用例**，而不是让 whole-suite 拖满 90 分钟超时后只报一句「Timeout」。这个设计值得保留。

**耗时基线（实测）**

| 项目 | 冷缓存 | 命中缓存 |
| --- | --- | --- |
| `Build & unit tests (ubuntu)` | 11m23s（需现场编译 vcpkg 依赖） | **5m47s ~ 7m34s** |
| `Code Quality`（7 项：tidy / cppcheck / 复杂度 / 格式） | — | 18m ~ 32m（clang-tidy 最慢） |

**Windows 本地交叉验证**（本机 Debug，`~[slow]` 全量）

| 目标 | 用例 | 断言 | 结果 |
| --- | --- | --- | --- |
| core | 197 | 821 | ✅ |
| agent | 880 | 3610 | ✅ |
| island | 44 | 237 | ✅ |
| tui | 253 | 4946 | ✅ |
| **合计** | **1374** | **9614** | **100% 通过**（墙钟 **3m1s**，2026-10-03 八次更新后实测） |

> 对比 2026-10-02 的 1366 / 9588：**+8 用例 / +26 断言**，全部来自 PR #116（#78 的 P4 命令探测、P3 接线、VF-08 stdin 三类新用例）。

全量构建 `cmake --build build --config Debug -j 8` 实测 **9m4s**。→ **两侧都全绿**，进一步支撑「测试套件本体健康，问题只在 CI 门禁的 flaky」。

**尚未覆盖**：评测基准（Terminal-Bench / SWE-bench）**一次都没跑过** —— 「评测与迭代」维度 45/100 即由此而来。当前环境跑不了：Docker daemon 未运行（CLI 已装）、Harbor / terminal-bench 未安装、且 30 题 × 3 次需要 API key 与可观成本。

---

## 3. 怎么给 Workx 真跑一次分（三步走）

### Step 1 — 加 headless 模式（前置，1–2 天）

```
workx -p "<task>" --output-format json     # 非交互单次执行，输出最终消息 + 统计
workx -p "<task>" --output-format stream-json
workx -p "<task>" --permission-mode accept-edits   # 无人值守档
```
要点：跳过 TUI/向导/索引、从 stdin 或 `-p` 取任务、结果写 stdout（JSON 含 turns / tool_calls / token / cost）、退出码语义化。
**验收**：能在无 TTY 的 CI/Docker 管道里跑通一条完整的 ReAct 链路。

### Step 2 — 写 Harbor adapter 并跑子集

```
agents/workx/
  __init__.py    # WorkxAgent(BaseInstalledAgent)：install() + @with_prompt_template run()
  README.md      # 跑分步骤 / 配置项 / 已知阻塞
scripts/harness/
  collect_metrics.py   # Step 2b 五项指标采集器（含 --self-test）
```

> ✅ **已落地（2026-10-03）**：适配器与采集器已写出，采集器 `--self-test` 5/5 通过，
> 并用真实 `workx -p ... --output-format stream-json` 输出验证过解析链路。
> ⚠️ **尚未实跑容器**：本机 Docker 守护进程未运行 + WSL 被安全策略拉黑，
> `install()` / `run()` 的容器内行为**未验证**，首次使用须先 smoke 单题。
>
> 🔴 **接口更正**：原计划的 `install.sh` / `run.sh` 两件套**已被 Harbor 废弃** ——
> Harbor 于 **2026-03-24** 做了破坏性重构，移除 `_install_agent_template_path` /
> `create_run_agent_commands` / `ExecInput` 与全部 `install-*.sh.j2` 模板，
> 现行契约是 `install(environment)` + `run(instruction, environment, context)`。
> 适配器按**现行**契约编写，老版本 Harbor 需按 CHANGELOG 反向迁移。

先跑 `terminal-bench@2.0` 的 **30 题随机子集 × 3 次**（对齐 HarnessTax 口径：任务平均后 bootstrap 10k 次取 95% CI），再逐步扩到全量 89 题。
模型选择上注意：Workx 主打 DeepSeek/GLM/Kimi，跑 SWE-bench 这类英文 Python 题会吃亏，建议**同时报一个 Claude/GPT 对照组**，才能把"harness 贡献"和"模型贡献"分开。

> ⚠️ **采集口径更正**：命令执行工具的真名是 **`Bash`**（`BashTool::name()`，Windows 另有 `PowerShell`），
> 不是本文早先假设的 `execute_command`。采集器按真名匹配，勿照旧口径写正则。

#### Step 2b — 一并验证 #78 的原始验收口径（行为统计，追踪 issue **#117**）

#78 已在 PR #116（`49d0ca4`）完整落地并关闭，但它的**原始验收标准是行为统计而非功能存在性**：

> 构造 10 个「实现某函数 + 有明确验收条件」的任务，统计 agent 在结束前**实际执行过测试/构建命令**的比例，目标 **≥ 90%**（改造前接近 0）。

这一条**至今未实测**（见 issue #78 关闭评论，`2026-10-02T14:54Z`）。它不能再靠单测证明——`[issue78]` 的 20 个用例验证的是「门禁被正确接线与判定」，不是「真实任务下 agent 真的会去跑命令」。因此**并入首轮跑分一并采集**：

| 采集项 | 口径 | 数据来源 |
| --- | --- | --- |
| **验证命令执行率** | 30 题中，agent 结束前执行过测试/构建命令的题数占比（≥90% 达标） | stream-json 的 step 序列，匹配 `tool_name == "Bash"`（Windows 另有 `PowerShell`）且命令命中测试/构建工具表 |
| **门禁触发率** | 其中**由门禁回灌驱动**（而非模型自选）的占比 | 日志中 `#78 verification not passed` → 后续 tool_call 的配对 |
| **降级率** | 达 `verify_max_attempts` 后带警告终止的题数 | `goal_status == Failed` 的题 |
| **误报率** | `unavailable` 直接放行的题数（推断不出命令） | 日志中 `#78 verification skipped (no command detected)` |
| **成本增幅** | 门禁默认开启后的平均 turns / token 增量 | 与关掉门禁的同题对照跑 |

> ✅ **前提已消解（2026-10-03 核实）**：`headless.cpp` 的 `step_json()` 在 `Action` 分支
> 已输出 `tool_name` + `tool_input`（含命令原文），`--output-format stream-json` 逐 step 落 NDJSON
> —— **采集不需要改 workx 代码**，早先「审计日志未导出该字段，要一并补」的判断不成立。
>
> ✅ **门禁日志已可用（#121 已修，PR #123 已合并）**：此前 headless **不初始化日志与审计** ——
> `src/tui/main.cpp` 的初始化块位于 headless 提前 return 之后，`WORKX_LOG_FILE` 被静默忽略、
> stderr 实测 0 字节，导致「门禁触发率 / 误报率」两项采不到。现把初始化提为
> `init_logging_and_audit(allow_default_file)`，headless 与交互式两条路径都显式调用；
> headless 侧**仅在显式指定路径时落盘**（不回落 `~/.workx/logs`，避免评测并发跑题互相覆盖）。
> 同时为 `audit.enabled` / `audit.file` 补上 `WORKX_AUDIT_ENABLED` / `WORKX_AUDIT_FILE`
> 环境变量绑定 —— 原先 `WORKX_AUDIT_FILE` **根本没有绑定**，设了也不生效。
>
> **判定规则**：首轮跑分后若验证命令执行率 **< 90%**，按实测结果**另开针对性 issue**（届时才有真实数据定位是「门禁未触发」「探测不到命令」还是「模型绕过」），不在本报告预先推测。验收追踪 issue：**#117**。

#### Step 2c — 首轮实测结果（2026-10-03，20 题 × 1 次）

| 项 | 值 |
| --- | --- |
| 模型 | DeepSeek `deepseek-v4-flash` |
| workx | 0.10.1（develop `1587fd0`，含 #82 环境上下文注入 + #126 门禁日志） |
| 题集 | terminal-bench@2.0 本地副本，**20 题 × 1 次**（`-k 1 -n 3 --force-build`） |
| 墙钟 | 1h49m（含每题本地构建镜像） |
| **pass@1** | **45.0%（9 / 20）** |
| 通过 | break-filter-js-from-html、cancel-async-tasks、chess-best-move、cobol-modernization、count-dataset-tokens、crack-7z-hash、db-wal-recovery、extract-elf、feal-linear-cryptanalysis |
| 失败 | adaptive-rejection-sampler、build-cython-ext、caffe-cifar-10、code-from-image、configure-git-webserver、custom-memory-heap-crash、extract-moves-from-video、feal-differential-cryptanalysis、financial-document-processor |
| 无分（异常） | filter-js-from-html（VerifierTimeoutError）、fix-code-vulnerability（EnvironmentStartTimeoutError） |

⚠️ **三个必须交代的口径问题**，否则这个 45% 会被误读：

1. **选题是容量排除，不是随机也不是挑难度。** 77 题本地副本里有 16 题的
   `[agent] timeout_sec > 1800`（`build-pov-ray` 12000s、`sam-cell-seg` 7200s、
   `compile-compcert` 2400s …），单机扛不住，先按**机器容量**排除，在剩下 61 题里
   **按字母序取前 20**。规则可复现，但**不是随机样本**，不能等同于全量 89 题的分数。
2. **尚未对齐 HarnessTax 口径**（任务先平均、再 bootstrap 10k 取 95% CI）。
   `-k 1` 单次采样，**没有置信区间**，只能当 smoke 之后的第一次量级确认。
3. **没有对照组。** 同一个 45% 里混着「harness 贡献」和「模型贡献」，
   报不出 harness 单独值多少 —— 必须再跑一组 Claude/GPT 对照才能分开。

**#117 行为统计口径（#78 的原始验收标准）：未达标。**

| 指标 | 实测 | 目标 | 分母 |
| --- | --- | --- | --- |
| 验证命令执行率 | **6.7%（1/15）** | ≥ 90% | 15（19 个 stream 里 4 个为空，已剔除） |
| 验证命令执行率（宽松，含自写测试脚本） | 6.7%（1/15） | — | 15 |
| 门禁触发率 | 0%（0/19） | — | 19 |
| 降级率 / 误报率 | 0% / 0% | — | 19 |

> ⚠️ **6.7% 不等于「agent 不验证」。** 逐条 dump 命令看过：`db-wal-recovery` 里 agent
> 写了 `# final verification read of main.db` 并真的查了 sqlite；`cobol-modernization` 里
> 反复跑程序比对输出。它**在验证，但不走任何可识别的测试/构建运行器** —— 用的是
> `python3 -c ...`、`od -c`、临时 `probe*.sh` 这类一次性探针。
> 所以严格口径（命中 `pytest` / `ctest` / `make test` / `npm test` … 运行器表）判不出来。
> **这是「验证行为没被规范化」的问题，不是「没有验证行为」的问题** —— 定位 issue 时要说清。

> 🔴 **门禁触发率 0% 是结构性的，不是 bug。** 这 20 题的工作目录 `/app` 里没有
> `CMakeLists.txt` / `package.json` / `Makefile` 之类的项目标记，`detect_default_goal()`
> 返回 None → #78 门禁完全不介入。7 个 trial 的日志里明确落了
> `#78 gate inactive (no goal under '/app'; set WORKX_GOAL/--goal)`（PR #127 新增的日志，
> 首轮 smoke 时这项是 0 次、无从判断）。**不要用这一项判达标/不达标。**

> ⚠️ **4 个 stream 是 0 字节**：agent 撞上任务超时被强杀 → 容器销毁 →
> `tee` 出来的 `/tmp/workx-stream.jsonl` 没取回。这 4 个 run **不是「没跑验证命令」，
> 是「不知道它跑了什么」**，已从验证类指标分母剔除；其中 3 个（cancel-async-tasks、
> extract-elf、break-filter-js-from-html）verifier 反而判了通过 —— 说明活干完了，
> 只是没在超时前收尾。

首轮实测同时暴露了采集器/导出器**两个真实缺陷**（已修，见 PR #128）：

1. `collect_metrics.py` 用 `rglob("*.jsonl")` 收 run，把同目录的 `workx-audit.jsonl`
   也当成了一次 run —— 20 题被算成 **38 个 run**，多出的 19 个审计文件里一条工具调用都没有，
   等于凭空灌进 19 个「没跑验证命令」的样本。
2. `export_run.py` 遇到 0 字节 stream 在 `lines[-1]` 处 IndexError 崩掉整个导出。
   现在改成写一份说明性轨迹并**不生成会话**（伪造空会话会让人误读成「agent 什么都没做」）。

### Step 3 — 建 trace 分析回路（持续迭代）

审计日志已有骨架，补一份 **run manifest**（每轮：工具名/参数/结果摘要/token/耗时/错误类型），然后按 LangChain 的做法：
拉取失败 run → 并行 spawn 错误分析 agent → 汇总成 harness 改动 → 复测。
**改完必须复跑，且每次改动都要看有没有在别的题上回归**（防过拟合到单题）。

### 参考分数线（预估，非实测）

| 阶段 | 预估 Terminal-Bench 2.x pass@1 | 实测 |
| --- | --- | --- |
| 补完 P0（headless + 深度限制 + schema 校验 + git 快照 + **#78 验证门禁 P1/P2/P3/P4**） | 45–58% | **45.0%**（20 题 × 1 次，2026-10-03，见 Step 2c） |
| 再补 P1（环境上下文注入 + 预算提示 + 沙箱） | 55–68% | 未测 |

> ⚠️ 45% 落在 P0 预估区间**下沿**，且该区间是按全量 89 题 + 多次采样估的，
> 本次是 20 题容量筛选子集 × 1 次（无 CI）。**不能据此说「已达成 P0 目标」**，
> 只能说量级对得上。要坐实需要：全量或更大随机子集 + `-k 3` + 对照组。

对照：LangChain 的 deepagents-cli 基线 52.8% → 优化后 66.5%（gpt-5.2-codex，同一模型）。Workx 在工具层更完整，但模型侧若用国产模型会有额外折让。

---

## 4. 改进优先级（ROI 排序）

```
P0-1 headless 模式          ← ✅ 已通（#77，PR #90）
P0-2 PreCompletionChecklist  ← ✅ 已完整落地并关闭（#78：P1/P2 PR #98 + P3/P4/VF-08 PR #116）
                              ⏳ 遗留：行为统计验收口径（≥90%）随首轮跑分采集（§3 Step 2b）
P0-3 子 Agent 深度 + 预算上限 ← ✅ 已修（#79，PR #92）
P0-4 schema 校验器补实现      ← ✅ 已修（#80，PR #90）
P0-5 git checkpoint/回滚      ← ✅ 已修（#81，PR #93）
🛠 阶段 6 CI 接入             ← ✅ 已落地并转**硬门禁**（PR #111 删 continue-on-error；1363 用例 100% 绿）
🔴 island 重连 flaky 修复      ← ⬅ **当前卡在这里**（#118；七次更新新发现：硬门禁 37.5% 误拦，同 commit 重跑翻转；
                                 根因 transport_posix.cpp accept 后关闭监听端点，重连窗口无界）
  ↓ 修完 flaky 再：跑第一轮 Terminal-Bench 子集，拿真实基线（比继续补 P1 更优先）
P1-1 环境上下文注入
P1-2 时间预算提示
P1-5 AGENTS.md 兼容        ← 3 行代码，立刻修
P1-6 PermissionMode 持久化
P1-3/1-4 Windows 沙箱 + 白名单
  ↓ 复测，画成本–成功率 Pareto 前沿
P2 文档对齐 / trace 导出 / token 计数校正 / 工具描述 eval
```

---

## 5. 参考来源

- [HarnessTax](https://harnesstax.github.io/) — UC Berkeley × Arena，21 组模型×harness 组合的成本/成功率 Pareto 分析
- [LangChain: Improving Deep Agents with Harness Engineering](https://www.langchain.com/blog/improving-deep-agents-with-harness-engineering) — 52.8% → 66.5% 的完整改动清单
- [LangChain: The Anatomy of an Agent Harness](https://www.langchain.com/blog/the-anatomy-of-an-agent-harness) — Harness 组件定义
- [Anthropic: Writing effective tools for agents — with agents](https://www.anthropic.com/engineering/writing-tools-for-agents) — 工具设计五原则 + eval 三步循环
- [Harbor](https://github.com/harbor-framework/harbor) — Terminal-Bench 官方 runner，支持自定义 agent
- [Agentic coding benchmarks in 2026, decoded](https://backgrind.com/blog/agentic-coding-benchmarks-2026) — 各基准治理方与陷阱
- [SWE-Bench vs Terminal-Bench: Governance First](https://www.digitalapplied.com/blog/swe-bench-terminal-bench-benchmark-guide-2026) — Verified/Pro 结构性差距、harness 10–20 分摆动
