# Workx Agent Harness 能力评估与改进路线

> 初评 2026-09-29 · **复审 2026-09-30**（develop `b2d41ca`）· **三次更新 2026-09-30 18:55**（develop `9a2de19`）· **四次更新 2026-10-01**（阶段 6：CI 接入）· **六次更新 2026-10-02**（CI 转硬门禁 + 平台缺陷清零）· **八次更新 2026-10-03**（#78 完整落地：P3/P4/VF-08，PR #116 `49d0ca4`）· **九次更新 2026-10-03（首轮真实跑分）**· 对象：`Workx v0.10.1`（`src/core` + `src/agent` + `src/tui`）
> 方法：**静态代码审计 + 业界 Harness 评测方法论对齐 + 首轮真实跑分**。
> ✅ **九次更新起，本文不再只是「预估分」**：已真的跑过一次基准 —— `terminal-bench@2.0` **20 题 × 1 次**，
> **pass@1 = 45.0%（9/20）**，墙钟 1h49m；#117 的五项行为统计指标已实测（**验证命令执行率 6.7%（1/15），未达 ≥90%**）。
> 🔴 **十次更新（2026-10-04）校准上面这个 45.0%**：它是 **harbor 的原始口径**，分母里混着 **5 个根本没有有效判定的 trial**
> （3 个 verifier 装不上 `uv`、1 个 verifier 超时、1 个环境容器起不来）。
> **只统计真正被判定过的 15 题，pass@1 = 60.0%（9/15）** —— 详见 **§2.6**。以下各处出现 45.0% 均指 harbor 原始口径。
> ⚠️ 但**只有三个维度拿到了实测证据**（验证闭环 / 可观测性与成本 / 评测与迭代），
> 其余四维仍是基于代码的评估 —— 一次 20 题、单次采样、无对照组的跑分**不足以支撑全表重估**。
> 详见 §2.4 与 §3 Step 2c。**P0 五项已全部完整落地**（含 **P0-2 #78 的 P1/P2/P3/P4 + VF-01~VF-11**，PR #116 `49d0ca4` 收尾，**issue 已于 2026-10-02 关闭**）。
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
>
> 🎯 **九次更新 2026-10-03（首轮真实跑分）**：**「评测与迭代」维度终于不是零了** ——
> Harbor adapter（`agents/workx/`）+ `terminal-bench@2.0` 题库（77 题本地副本）+ 采集器与导出器
> 全部落地，并**真的跑完 20 题**：**pass@1 = 45.0%（9/20）**，墙钟 1h49m，模型 DeepSeek `deepseek-v4-flash`。
> 三个维度因此拿到实测证据（**其余四维无新证据，维持不变**）：
>
> - **验证闭环 70 → 65**：#117 实测**验证命令执行率 6.7%（1/15）**，远未达 ≥90% ——
>   从「机制完整但效果待证」变成「**效果已证且未达标**」。
>   ⚠️ **但不降到更低**：门禁机制本身**没被证伪** —— 0% 触发是**结构性**的（`/app` 里没有
>   `CMakeLists.txt` / `package.json` / `Makefile`，`detect_default_goal()` 返回 None，日志落
>   `#78 gate inactive`），且 agent **确实在自检**（实测用 `python3 -c` / `od -c` / 临时 `probe*.sh`），
>   真问题是**验证行为没规范化成可识别的构建/测试命令** → 已开 **#132**。
> - **可观测性与成本 78 → 82**：常驻导出器 `scripts/harness/export_run.py` 落地（可读轨迹 + 可导入会话），
>   并拿到真实成本数据（均值 prompt 52,145 / generated 898 tokens，425s / run，32.5 次工具调用）。
>   不给更高：**4/20 的轨迹是 0 字节**（agent 撞超时被杀、产物没取回），观测仍有 20% 盲区。
> - **评测与迭代 45 → 72**：从「无 adapter、无 eval 集、未跑过任何基准」到**三者齐备**。
>   不给满分：`-k 1` 单次采样无置信区间、无对照组、选题是容量排除后按字母序取前 20（**非随机**）、
>   Step 3 的 trace 分析回路还没真正跑起来。
>
> → **加权总分 72.9 → 73.5**。📌 顺带抹平一处记账差：主文档记 72.9、评分卡 HTML 记 72.7，
> 按表内分值重算**八次实为 72.7**，本次统一。

---

## 0. 一句话结论

Workx 的**工具层和安全层**完成度相当高（22 个工具、并发执行、fail-closed 权限、密钥脱敏、四级缓存感知压缩、MCP、Hooks、Plan mode 双防线），在自研 harness 里属于上游水平；
**验证闭环（Verification Loop）此前几乎空白，现已完整落地 P1 / P2 / P3 / P4**（主循环门禁 + headless 与交互式均默认开启 + 命令自适应探测），**非交互执行模式也已打通**（#77）；
但**仍没有任何能力评测集**——而业界 2026 年的共识恰恰是：这三项才是拉开 harness 差距的地方（LangChain 仅靠补验证闭环 + 环境上下文，Terminal-Bench 2.0 从 52.8% → 66.5%，+13.7 分，模型一行没改）。

**加权总分：73.5 / 100**（初评 61.05 → 复审 68.5 → 二次更正 69.9 → 三次更新 71.9 → 七次 71.7 → 八次 72.7（原记 72.9，九次按列值重算抹平 0.2）→ **九次更新 73.5**；七次依据真实 CI 数据微调、八次依据 #78 完整落地、**九次依据首轮真实跑分**；详见 §2 / §2.4）

**复审一句话**：P0 五项**已全部完整落地**——前四项把「能不能测」这个前置问题解决了；**P0-2（#78）已完成 P1 / P2 / P3 / P4 与 VF-01~VF-11**（PR #116 `49d0ca4` 收尾并关闭 issue），验证闭环既跑在评测入口 headless 上，也跑在真实用户路径交互式上，且命令改为按项目线索自适应探测。**唯一未竟的是它原始的行为统计验收口径（≥90%），已并入首轮跑分采集（§3 Step 2b）。**
初评「补齐 P0 后约 82 分」的预估仍偏乐观，实际 **72.9** —— 因为「评测与迭代」维度的分数取决于**真的跑过分**，而这件事至今一次都没做。
**七次更新一句话**：真的把 CI 跑起来核对了 —— **1363 个单测确实全绿**（同 commit 重跑独立复现），但**硬门禁本身有 37.5% 的 flaky 误拦**，`develop` 曾因此连续两次变红。**「有真实数据」和「数据可信」是两件事**。

**九次更新一句话**：**真的跑了一次基准，然后发现「能跑」和「跑得好」之间还隔着一整套方法论** ——
pass@1 **45.0%（9/20）** 落在 P0 预估的下沿，但它是 `-k 1`、无对照组、非随机选题的产物，**不能据此说达成**；
更要紧的是 #117 实测**验证命令执行率只有 6.7%**，而深挖下去发现根因不是「agent 不验证」，
而是**验收口径与 Terminal-Bench 的场景根本不匹配**（verifier 测试跑在独立容器，agent 容器内拿不到 `tests/`）。
→ 跑分最大的价值不是那个 45%，而是**把三个此前只能靠猜的维度换成了实测**。
进一步做失败归因（**§2.5**）后还有一层：**真能力失败只有 3 题** ——
反而有 **9/19 题的终止路径都经过一个已失效的内部评审器**（其中 3 题纯属误杀，**#133**），
另有 3 题因 verifier 依赖外网下载 `uv` 而**环境性不可解**（剔除后 **52.9%**）。
「模型不够强」是这里面占比最小的原因。

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

| # | 维度 | 权重 | 初评 | 复审 | 三次 | 八次 | **九次** | 加权 | 变动依据 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 工具设计与执行 | 20 | 72 | 82 | 82 | 82 | **82** | 16.4 | **#80**：`json_schema.{h,cpp}` 275 行实现，接入 `executor.h:223` 作 `validate_input` 后兜底；新工具漏写不再裸奔。CI 实测（含 `[tool][grep][search]` 5 项，修 #107 后）全绿。🎯 **九次无变动**：20 题实测均值 32.5 次工具调用 / run，无工具层故障证据 |
| 2 | 上下文工程 | 20 | 70 | 70 | 70 | 70 | **70** | 14.0 | 未动（#82 环境上下文注入未做）。🎯 **九次无变动**：跑分未产生该维度的新证据 |
| 3 | **验证闭环** | 20 | 40 | 55 | 65 | 70 | **65** | 13.0 | **#78 已完整落地**（P1/P2 `9a2de19` + P3/P4/VF-08 `49d0ca4`）：门禁进主循环、headless 与**交互式均默认开启**、命令自适应探测且 `unavailable` 与失败分离 → 八次较七次 **+5**。<br>🔴 **九次 -5**：#117 实测**验证命令执行率 6.7%（1/15）**，远未达 ≥90% ——「效果待证」变成「**效果已证且未达标**」。<br>⚠️ **但不降到更低**：机制本身未被证伪 —— 0% 触发是**结构性**的（`/app` 无工程标记 → `detect_default_goal()` 返回 None，日志落 `#78 gate inactive`），且 agent **确实在自检**（`python3 -c` / `od -c` / `probe*.sh`）。真根因是**验证行为未规范化成可识别的构建/测试命令** → **#132** |
| 4 | 安全与权限 | 15 | 68 | 75 | 75 | 75 | **75** | 11.25 | **#81**：基线 commit + 人工回滚入口（只读安全网）+ #88 文件权限 0600 + #84 沙箱降级可见化 / Job Object；白名单档（#85）仍未修。🎯 **九次无变动**：跑分未产生该维度的新证据 |
| 5 | 编排与可靠性 | 10 | 62 | 72 | 72 | 70 | **70** | 7.0 | **#79**：`SubAgentBudget` 批量 + run 累计双护栏、atomic CAS 预约。🔴 **七次扣 2 分**：真实 CI 数据实测硬门禁 **37.5% flaky 误拦**（同 commit 重跑翻转）—— 该维度含「可靠性」，而 CI 门禁是本项目**唯一**的自动化回归防线。🎯 **九次无变动**：20 题跑通无崩溃，但 `-k 1` 单次采样不足以评估编排稳定性 |
| 6 | **可观测性与成本** | 10 | 75 | 78 | 78 | 78 | **82** | 8.2 | headless json 输出带 `usage`；收尾报告含 `git_diff_summary`。<br>✅ **九次 +4**：常驻导出器 `scripts/harness/export_run.py` 落地（可读轨迹 Markdown + **可导入 workx 原生会话**），补上 P2-1「无 run trace 导出」；并拿到**真实成本数据**（均值 prompt 52,145 / generated 898 tokens、425s / run、32.5 次工具调用；账单实测 ¥0.21 / 64 次 / 522,060 tokens）。<br>⚠️ **不给更高**：**4/20 的轨迹是 0 字节**（agent 撞超时被杀、容器销毁、`tee` 产物没取回），观测仍有 **20% 盲区** —— 跑得最久、最可能撞超时的题恰恰最看不到 |
| 7 | **评测与迭代** | 5 | 15 | 45 | 45 | 45 | **72** | 3.6 | 八次的依据是「headless 已通，前置解锁；但**无 adapter、无 eval 集、未跑过任何基准**」。<br>✅ **九次 +27（本轮最大变动）**：**三者齐备并真的跑完一轮** —— Harbor adapter（`agents/workx/`）+ `terminal-bench@2.0`（77 题本地副本）+ `collect_metrics.py` 五项指标采集器，`harbor run` 实测 **pass@1 45.0%（9/20）**，墙钟 1h49m。<br>⚠️ **不给满分（72 而非 90+）**：`-k 1` 单次采样**无置信区间**、**无对照组**（45% 里 harness 与模型的贡献分不开）、选题是容量排除后**按字母序取前 20（非随机）**、Step 3 的 trace 分析回路尚未真正跑起来 |
| | **合计** | 100 | 61.05 | 68.5 | 71.9 | 72.7 | **73.5** | — | 二次更正 48→55 是记账更正；三次 55→65 是 #78 P1/P2 的真实提升；七次 71.9→71.7 是真实 CI 数据对门禁可靠性的修正；八次 71.7→72.7 是 #78 完整落地（P3 交互式默认开启为真实用户路径补上验证）；**九次 72.7→73.5 是首轮真实跑分**（评测 +27 / 可观测 +4 / 验证闭环 -5）。<br>📌 八次合计原记 72.9，按表内分值重算实为 **72.7**，本次抹平 —— 主文档与评分卡 HTML 此前差 0.2 |

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

**评测基准覆盖情况**：~~一次都没跑过~~ → ✅ **已于 2026-10-03 跑完首轮**（`terminal-bench@2.0` **20 题 × 1 次**，**pass@1 45.0%**），详见 **§2.4**。此前「评测与迭代 45/100」正是由「一次都没跑过」而来，该分值在九次更新中已上调至 72。

---

### 2.4 首轮跑分实测数据（九次更新，2026-10-03）

> 以下全部取自 `harbor run` 的**真实产物**（`jobs/2026-10-03__13-45-16/`），不是代码审计推断。
> 这是 Workx **历史上第一次跑完评测基准**。

**跑分条件**

| 项 | 值 |
| --- | --- |
| 题库 | `terminal-bench@2.0`，本地副本 77 题（`.cache/tb2/terminal-bench`） |
| 规模 | **20 题 × 1 次**（`-k 1 -n 3 --force-build --environment-build-timeout-multiplier 4`） |
| 模型 | DeepSeek `deepseek-v4-flash` |
| workx | 0.10.1，构建自 develop `1587fd0` |
| 墙钟 | **1h49m39s**，`HARBOR_EXIT=0` |

**结果：pass@1 = 45.0%（9 / 20）** ← harbor 原始口径，含 5 个无有效判定的 trial

> 🔴 **校准（2026-10-04，#142）**：上表的 9 题「失败」里有 3 题是 verifier 自己崩了（装不上 `uv`），
> 另有 2 题「异常」分别是 verifier 超时和环境容器起不来。**真正被判定过的只有 15 题**：
> **pass@1 = 60.0%（9 / 15）**，verifier 失效率 **25.0%（5/20）**。详见 **§2.6**。

| 结果 | 题数 | 题目 |
| --- | --- | --- |
| ✅ 通过 | **9** | break-filter-js-from-html、cancel-async-tasks、chess-best-move、cobol-modernization、count-dataset-tokens、crack-7z-hash、db-wal-recovery、extract-elf、feal-linear-cryptanalysis |
| ❌ 失败 | 9 | adaptive-rejection-sampler、build-cython-ext、caffe-cifar-10、code-from-image、configure-git-webserver、custom-memory-heap-crash、extract-moves-from-video、feal-differential-cryptanalysis、financial-document-processor |
| ⚠️ 异常 | 2 | filter-js-from-html（VerifierTimeoutError）、fix-code-vulnerability（EnvironmentStartTimeoutError，**压根没跑**） |

> 🔎 **这 45% 是怎么丢的，见 §2.5**：实证归因显示**真能力失败只有 3 题**；
> 9/19 的终止路径都经过一个已失效的内部评审器（其中 3 题纯属误杀），
> 另有 3 题因 verifier 依赖外网下载 `uv` 而**环境性不可解**。
> → **剔除那 3 道不可解题后 pass@1 = 9 / 17 = 52.9%**。

**#117 五项行为统计指标**

| 指标 | 实测 | 分母 | 证据源 |
| --- | --- | --- | --- |
| **验证命令执行率** | **6.7%（1/15）** | 15 | stream |
| 验证命令执行率（宽松） | 6.7%（1/15） | 15 | stream |
| 门禁触发率 | 0%（0/19） | 19 | log |
| 降级率 | 0%（0/19） | 19 | stream |
| 误报率 | 0%（0/19） | 19 | log |

分母 20 → 19：`fix-code-vulnerability` 环境启动超时没跑。19 → 15：**4 个 stream 是 0 字节**
（agent 撞超时被杀、容器销毁、`tee` 产物没取回）。这 4 个**不是「没跑验证命令」，是「不知道它跑了什么」**，按诚实性原则剔除。

**成本实测**

| 成本项 | 均值 |
| --- | --- |
| total_iterations | 27.7 |
| total_tool_calls | 32.5 |
| prompt_tokens | 52,145 |
| generated_tokens | 898 |
| duration_ms | 425,206（约 7 分钟 / run） |

账单侧独立核对（DeepSeek 控制台）：**¥0.21 / 64 次请求 / 522,060 tokens** —— 与采集器口径一致。

**⚠️ 三个必须交代的口径偏差**（写任何结论时都要带上）

1. 选题是**容量排除**不是随机：77 题里 16 题的 `[agent] timeout_sec > 1800`，单机扛不住，
   排除后在剩下 61 题里**按字母序取前 20**。
2. `-k 1` **单次采样**，无置信区间，未对齐 HarnessTax 口径（它要求多次重复 + bootstrap CI）。
3. **无对照组**：45% 里 harness 贡献与模型贡献分不开。

→ 所以 **45.0% 只能说明「能跑通」，不能据此说「达成目标」**。它落在 P0 预估的下沿。

**🔴 6.7% 的真实含义（别误读成「agent 不验证」）**

逐条 dump 命令后看到的：

- `db-wal-recovery`：写了 `# final verification read of main.db` 并真的执行 sqlite 查询
- `cobol-modernization`：反复编译运行、比对输出、写 `probe*.sh` 做差分测试

**它在验证，但不走任何可识别的测试/构建运行器** —— 用的是 `python3 -c`、`od -c`、临时 `probe*.sh` 这类一次性探针。

更根本的原因：**Terminal-Bench 的 verifier 测试跑在独立的 verifier 容器里，agent 容器内拿不到 `tests/`** ——
所以「跑可识别的测试运行器」这个验收口径在这套题上**先天不适用**。
→ **#117 的 ≥90% 是按「真实工程仓库（有构建系统）」设定的**，套到「空目录做题」会系统性失真。
已按判定规则另开 **#132**，并在其中提出「口径分场景」的建议。

**门禁触发率 0% 是结构性的，不是 bug**

`/app` 里没有 `CMakeLists.txt` / `package.json` / `Makefile` → `detect_default_goal()` 返回 None →
#78 门禁完全不介入。7 个 trial 的日志明确落了 `#78 gate inactive (no goal under '/app'; set WORKX_GOAL/--goal)`
（PR #127 新增的日志）。**不要用这一项判达标/不达标。**

**产物与工具**

- 逐题索引：`jobs/2026-10-03__13-45-16/export/RESULTS.md`
- 指标报告：`jobs/2026-10-03__13-45-16/export/metrics.md`
- 19 份可读轨迹 + 15 份可导入会话（已装入 `~/.workx/projects/terminal-bench-<题名>/`）
- 常驻工具：`scripts/harness/export_run.py`（导出）、`scripts/harness/collect_metrics.py`（采集）

> 📎 **本节是评分卡视角的摘要**。执行过程、采集器/导出器在真实数据下暴露的三个缺陷、
> 「38 → 53 → 19」的 run 计数踩坑，见 **§3 Step 2c**。

---

### 2.5 首轮失败归因：45% 是怎么丢的（九次更新补记）

> 用 19 份轨迹 + 逐题日志做的**实证归因**，不是推测。
> 结论：**真正「模型做不出来」的只有 3 题**，其余丢分来自 harness 自身与环境。

**按终止方式归因**

| 终止方式 | 题数 | 通过 |
| --- | --- | --- |
| 🔴 内部评审器失效 → `wrap_up` | **9** | 3 |
| 轨迹丢失（撞超时被杀，0 字节） | 4 | 3 |
| 正常完成 | 5 | 3 |
| 请求超时（`Total request timeout exceeded`） | 1 | 0 |

#### 🔴 头号原因：内部评审器 100% 失效，且失效时默认终止任务

19 题里 **9 题**的日志都有这一行（`react_loop.cpp:541`）：

```
[react_loop] reviewer: no JSON decision in response, default wrap_up
[react_loop] reviewer decision: wrap_up, reason=''
```

| 触发评审器的原因 | 题数 | 结果 |
| --- | --- | --- |
| 撞最大迭代预算（40 轮） | 6 | 3 通过 / 3 失败 |
| 检测到重复工具调用（stall） | 3 | **0 通过 / 3 失败** |

被 stall 分支终止的 `build-cython-ext`（iteration 27）/ `caffe-cifar-10`（34）/
`custom-memory-heap-crash`（26）**无一通过**。它们**不是做错被判失败**，而是在第 26–34 轮
因一次「重复工具调用」触发评审器 → 评审器失效 → 默认 `wrap_up` → **任务就地终止**。
末段调用是在正常推进（`Edit 文件` → `Read 同一文件` 验证改动），不是死循环。

**根因链**

```cpp
// src/agent/core/react_loop.cpp:498 —— 评审器只给 200 tokens
req.max_tokens = 200;
// :520-542 —— 找不到 '{' 说明 content 是空的
const auto lbrace = text.find('{');
else LOG_WARN("... reviewer: no JSON decision in response, default wrap_up");
```

主循环日志实测该模型的 reasoning 规模：`reasoning_len=87267`（87K 字符）、
单次 `thought_ms=120009`（120 秒）。**thinking 模式下 200 tokens 会被 reasoning 一口吃光**
→ `content_delta` 全程为空 → `text` 空 → 找不到 JSON → `ReviewerDecision` 默认
`continue_loop=false`（`react_loop.cpp:469`）→ **整条任务被杀**。

> 📌 **这是 fail-deadly**：评审器的职责是判断「**是否可以**停止」，它失效时却默认「停」。
> 安全默认应该是 `continue`，让 `max_iterations` / 预算去兜底终止。
> 旁证：`db-wal-recovery` 的最终答复本身就是
> `HTTP error: 400 - reasoning_content in the thinking mode must be passed back to the API`
> —— 该 provider 的 thinking 模式在协议层就有问题。→ 已开 **#133**（P1/bug）。

#### 第二：3 题环境性不可解（verifier 依赖外网）

`adaptive-rejection-sampler` / `caffe-cifar-10` / `feal-differential-cryptanalysis`
的 verifier `test.sh` 要用 `uvx`，容器内连不上 github：

```
curl: (7) Failed to connect to github.com port 443 after 21081 ms
failed to download https://github.com/astral-sh/uv/releases/download/0.9.5/...
/tests/test.sh: line 19: uvx: command not found
```

**这 3 题 agent 做对了也判不过** —— 属环境性必然失败，不该记在 harness 账上。
→ **剔除后 pass@1 = 9 / 17 = 52.9%**。

#### 第三：真能力不足只有 3 题

| 题 | verifier 判定 |
| --- | --- |
| `code-from-image` | `File /app/output.txt does not exist` |
| `extract-moves-from-video` | `File /app/solution.txt does not exist` |
| `configure-git-webserver` | `assert 'TEST PASSED' in ...` 输出不对 |

这 3 题是货真价实的能力失败。

#### 两个反直觉的点

1. **通过题均 92 步 vs 失败题均 89 步 —— 几乎一样**。失败不是「做得少」，
   两类题都跑到了 ~90–130 步才结束。
2. **命令层面零重复**：脚本查过 19 题，窗口 4 内完全相同的命令 **0 处**。
   stall 触发的是 Read/Edit 类调用，而 `commands` 字段只记 Bash ——
   **光看命令看不出来，必须查日志**。

#### 顺带否掉的两个假设

- ❌ **「思考太慢导致做不完」**：536 次思考**中位 2296ms**，>60s 仅 6 次（1%）。
  速度不是瓶颈，40 轮预算是够用的。
- ❌ **「`normalize_tool_input` 把不同命令归一化成了同一个签名」**：实现是对的
  （标量走 `j.dump()`，只忽略键顺序），不是误判来源。

#### 修复优先级（详见 #133）

1. **评审器失效时默认 `continue` 而非 `wrap_up`** —— 最小改动、收益最大。
2. 评审器请求关掉 thinking，或把 `max_tokens` 提到 512–1024（它只是输出一行 JSON 的判断任务）。
3. `review_stall_window = 4`（`react_loop.h:198`）太小，建议 8–10，且要求**连续**重复才触发。
4. 单独修 `reasoning_content` 的 provider 适配 400 错误。

#### ✅ 修复已合入（2026-10-03，develop `84334aa` / PR #135）

| 项 | 落地情况 |
| --- | --- |
| 1. fail-open | ✅ `ReviewerDecision::valid`：只有明确解析出 `continue`/`wrap_up` 才算可信裁决；失效一律继续，终止权交回预算 |
| 2. max_tokens | ✅ 200 → **512**，提为 `Config::review_max_tokens`（原硬编码在 `run_reviewer` 内） |
| 3. 停滞窗口 | ✅ 默认 4 → **8**；「要求连续重复」**未采纳**（见下） |
| 4. provider 400 | ➡️ 另开 **#136**，不在本次改动范围 |

配套加了**评审器熔断**：连续失效达 `max(1, review_max_grants)` 次后不再调用，
直接 fail-open —— 实测 9/9 失效，不熔断就是每个停滞点白烧一次注定无果的 LLM 请求。

日志侧同步增强：无 JSON 时打印 `content={}B, reasoning={}B`（一眼看出是不是推理链
吃光预算），`reviewer decision` 带 `valid=` 标记。

**未采纳「要求连续重复才触发停滞」的理由**：若改成只比较 `recent_calls.back()`，
窗口大小就不再影响判定，`review_stall_window` 会变成死旋钮；且 fail-open 已经消除了
误判的致命后果（误判现在只花 1 轮迭代 + 1 次评审调用，不再杀任务），窗口 4→8 已足够。

验证：新增 5 个单测；Windows 本地 Debug `[react_loop]` 45 用例 / 187 断言、
`~[slow]` 全量 **912 用例 / 3727 断言** 全绿无回归。CI 硬门禁 `Build & unit tests` 7m26s pass。

⚠️ **评分卡暂不上调**：45.0% → 多少必须等复跑数据，不靠推理给分。

#### 🔁 复跑验证：3 题被误杀的任务（2026-10-03，job `2026-10-03__21-05-15`）

修复合入后重跑当初被 stall 分支杀掉的 3 题（同题、同模型、`-k 1 -n 3`，
新二进制 `workx-linux-amd64` 10.0MB，glibc 2.35 配方）。

| 题 | 首轮（旧） | 复跑（新） | reward |
| --- | --- | --- | --- |
| `build-cython-ext` | iteration=27 被 stall→wrap_up 杀 | iteration=37，**900s 墙钟超时** | 0 → **0** |
| `custom-memory-heap-crash` | iteration=26 被杀 | iteration=34 **正常给出 final_answer**（202s / 43 次工具调用） | 0 → **0** |
| `caffe-cifar-10` | iteration=34 被杀 | iteration=32，**1200s 墙钟超时** | 0 → **0** |

**pass@1：0/3 → 0/3，没有变化。**

##### ⚠️ 但这次复跑不能作为「修复有效」的证据

日志里 `reviewer` 与 `stall` 的命中数**都是 0** —— 评审器一次都没被调用过。
也就是说：

- fail-open 路径**根本没被触发**；
- 首轮那 3 次 stall 是**轨迹偶发**（LLM 每次跑法不同，这轮没出现重复工具调用）；
- 因此 `build-cython-ext` 从「10 failed / 1 passed」变成「2 failed / 9 passed」
  **不能归因于本次修复**，只能归因于跑法差异。

能确证的只有三件事：

1. ✅ **新二进制确实在用**：日志源码行号整体位移
   （`react_loop.cpp:811 → 846`、`954 → 无`、`1334 → 1446`）。
2. ✅ **没有任何任务再被评审器杀掉**；`custom-memory-heap-crash` 跑到了自然结束并给出真实 final_answer。
3. ✅ 无崩溃、无回归，长跑（34/37 轮）稳定。

##### 🔴 新暴露的瓶颈：墙钟超时取代了评审器终止

3 题里 **2 题由 `AgentTimeoutError` 结束**（900s / 1200s）。
fail-open 的代价很直接：以前评审器在第 27 轮就把任务掐了（省下时钟），
现在任务会一直跑到**撞墙钟**为止。对注定做不出来的题，分数不变、时间变长。

`build-cython-ext` 是最说明问题的一题：pytest **9/11 通过**（首轮只有 1/11），
剩下的 2 个失败是 `test_repo_cloned` / `test_pyknotid_repository_tests`
—— 要 `/app/pyknotid` 带 `.git`，即**需要联网 clone**。
**它是被时钟和/或网络卡住的准成功题，不是能力题。**

##### 方法论警告：verifier 环境不稳定，跨轮对比有噪声

`custom-memory-heap-crash` 首轮 verifier 是正常 pytest（4 passed / 2 failed），
**复跑时 verifier 自己装不上 `uv`**（`curl: (56) Failure when receiving data from the peer`，
从 github 下载 uv 失败）→ 这一题在复跑里**根本不可能得分**。

→ 「环境性不可解」不是固定集合，而是**每次跑都可能变**的噪声源。
首轮剔除 3 题算出的 52.9% 同样受此影响，**不要把调整后的分数当硬结论**。

##### 下一步建议

1. **抬时钟而不是抬轮数**：现在卡点是墙钟，不是 `max_iterations`。
   优先做「单轮更快」（并行工具、减少重复读文件）或按题调 `[agent] timeout_sec`。
2. **复跑要 `-k 3`**：n=1 且评审器触发本身是偶发的，单次跑分无法区分
   「修复有效」和「这次没撞上」。
3. 见 **#137**（墙钟超时成为新瓶颈）。

#### 2.6 三组对照实验：时间、轮数、封顶到底谁在起作用（2026-10-04）

上一节的「下一步建议」在 2026-10-04 全部执行了，得到了比预想更细的结论。
同一 3 题（`build-cython-ext` / `financial-document-processor` / `configure-git-webserver`），
三种配置，每组 `-k 2`：

| 配置 | `--agent-timeout-multiplier` | 墙钟预算注入 | 代表 job |
| --- | --- | --- | --- |
| A 首轮基线 | 1.0 | 无 | `2026-10-03__13-45-16` |
| B 标准时钟 + 封顶 | 1.0 | `WORKX_AGENT_TIMEOUT_SEC=900` | `2026-10-04__14-11-12` |
| C 抬时钟 | **2.5** | 无 | `2026-10-04__13-50-37` |

以 `build-cython-ext` 为例（它的 verifier 环境全程正常，数据最干净）：

| 配置 | 结束轮数 | pytest 结果 |
| --- | --- | --- |
| A 首轮 | it=27（被评审器 stall 误杀） | **1 passed / 10 failed** |
| B 标准 + 封顶 | it=40 / it=49（两次一致） | **9 passed / 2 failed** |
| C 抬时钟 2.5× | it=37 / it=45 | **10 passed / 1 failed** |

**Pass@2 三组都是 0.000** —— 因为 Terminal-Bench **全对才给 1 分**，
9/11 与 1/11 同价。这是本轮最重要的认知：

> **改善「进展」和改善「分数」是两件事。**
> 我们能稳定把 1/11 提到 9~10/11，但剩下那 1~2 项卡在**联网 clone 仓库**上，
> 在容器网络受限的前提下**不可能完成** → 分数纹丝不动。

##### 逐个变量的实际贡献

1. **#133 的 fail-open：消除退化尾部，不是提高上限**
   首轮 `build-cython-ext` 在 it=27 被 stall 误判杀掉 → 只完成 1/11；
   修复后不再可能被误杀，稳定跑到 37~49 轮 → 9~10/11。
   价值在于**「1/11 那种退化不会再发生」**，而不是「把上限从 9 提到 10」。
   （上一节判断「复跑 0/3 = 修复无效」是错的 —— 那次它撞了墙钟，掩盖了收益。）
2. **抬时钟有效，但换不来分数**：2.5× 把失败项压到 1，仍差最后一步。
3. **墙钟感知封顶（PR #141）：本轮 0 次触发，收益未证**
   日志里 `remaining wall clock` 命中数为 **0** —— 这几题 agent 没请求长超时，
   首轮那条 600s 命令是**偶发**（agent 显式设了 `timeout=600000`）。
   改动本身是低成本、默认关闭的防御（针对真实观测过的失败模式），
   但**不能宣称收益**，必须在有长超时命令的场景下重测。

##### 🔴 比上面三条更严重：50% 的 trial 数据根本无效

B 组 6 个 trial 里 **3 个的 verifier 自身崩溃**，测试压根没跑：

```
curl: (7) Failed to connect to github.com port 443 after 21037 ms
failed to download .../uv-x86_64-unknown-linux-gnu.tar.gz
/tests/test.sh: line 15: uvx: command not found
```

这些 trial 的 `reward.txt` 写的是 **0**，于是被统计成「agent 失败」。
**实际上 agent 什么都没被判定过。** 首轮 `custom-memory-heap-crash` 也是同一病症。

→ 这句「无法回溯」已被推翻：**#142 的采集器做出来了，噪音已能量化**。
   详见 **§2.6** —— 首轮 20 题里 **5 题（25.0%）没有被真正判定过**，
   去噪后 **45.0% → 60.0%（9/15）**。

### 2.6 分数去噪：到底有多少分是被「没判定」坑掉的（十次更新，2026-10-04）

#142 的产物 `scripts/harness/trial_metrics.py`（`--self-test` 26 项自检全绿）。

**判定方法**：不看 `reward.txt`（那是 harbor 的结论），而是看 **verifier 有没有真的跑完** ——
以 `verifier/ctrf.json` 为第一判据，辅以 pytest 摘要行、`uv` 安装失败串、超时标记。
`reward.txt = 0` 且 verifier 没跑完的 trial，标记为**无有效判定**，从分母里剔除。

> ⚠️ 关键设计：**「无有效判定」≠「verifier 坏了」**。它进一步分成两类归因，
> 因为「容器压根没起来」时 agent 可能**根本没跑**，把它记在 verifier 账上会让诊断跑偏。

#### 首轮 20 题 job（`2026-10-03__13-45-16`）去噪结果

| 口径 | 值 | 计数 |
| --- | --- | --- |
| pass@1（全部 trial，**= harbor 原始口径**） | 45.0% | 9 / 20 |
| **pass@1（有效 trial）** | **60.0%** | **9 / 15** |
| 无有效判定率 | 25.0% | 5 / 20 |
| ├ `verifier` 类（依赖装不上 / 没跑完） | 20.0% | 4 / 20 |
| └ `environment` 类（容器起不来 / 启动超时） | 5.0% | 1 / 20 |

**45.0% 与 60.0% 都是真的**，差别只在分母：前者是 harbor 的原始口径（含 5 个从未被判定的 trial），
后者是「真正做过判定的题」里的通过率。**对外沟通用哪个必须先说清口径。**

#### 全部 11 个 job（52 trial）汇总

| 类别 | trial 数 | 占比 |
| --- | --- | --- |
| `ok`（verifier 真跑完并给分） | 29 | 55.8% |
| `verifier` 类失效 | 12 | 23.1% |
| `environment` 类失效 | 11 | 21.2% |
| `unknown`（需人工看日志） | 0 | 0.0% |

| 口径 | 值 | 计数 |
| --- | --- | --- |
| pass@1（全部 trial，原始口径） | 21.2% | 11 / 52 |
| **pass@1（有效 trial）** | **37.9%** | **11 / 29** |

> 跨 job 的 52 题**不能当成一次跑分看**：里面是 11 次不同目的的作业（smoke / 复跑 / 对照组），
> 同一道题重复出现。它只说明「失真现象普遍存在」，**不代表 workx 的真实水平**。

#### 🔴 一个可直接行动的发现

`verifier` 类失效里最大单项是 **`verifier_infra:uv_download`（7 次）** —— 全部是同一个错：

```
failed to download https://github.com/astral-sh/uv/releases/download/0.9.5/uv-x86_64-unknown-linux-gnu.tar.gz
curl: (7) Failed to connect to github.com port 443 after 21081 ms
```

**这是宿主机网络问题，不是 workx 的问题，也不是 verifier 脚本的问题。**
2026-10-04 15:45 复测：宿主机到 github 已完全恢复（代理与直连均 10/10 通过，
`pypi.org` 2.97MB 抓取正常）。→ **复跑时这 7 个 trial 应当自动转为有效**，
预期「`verifier` 类失效率」会显著下降，这是验证修复是否生效的最直接观测点。

#### 用法

```bash
python scripts/harness/trial_metrics.py jobs/<job-时间戳> [--md out.md] [-o out.json]
python scripts/harness/trial_metrics.py --self-test   # 合成样例自检
```

后续跑分**必须先跑它再去对比数字**，否则是在噪声上做决策。

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

> 📎 **评分卡视角的摘要见 §2.4**（总分 72.7 → 73.5 即由此而来）。本节是执行记录，
> 含三个口径偏差的细节与工具缺陷的修复过程。

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

> ✅ **已按判定规则落地（2026-10-03）**：实测结果已回写 **#117** 并**保持 OPEN**（6.7% < 90%，不达标就不关）；
> 同时按规则另开针对性 issue **#132** —— 定位是「**验证行为未规范化成可识别的构建/测试命令**」，
> 而非「agent 不验证」；根因含一条更根本的：**Terminal-Bench 的 verifier 测试跑在独立容器里，
> agent 容器内拿不到 `tests/`**，故 ≥90% 这个口径与「空目录做题」场景先天不匹配。
> #132 中提出的「口径分场景」建议尚未拍板，**#117 的判定标准暂未改动**。

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

首轮实测同时暴露了采集器/导出器**三个真实缺陷**（已修，见 PR #128）：

1. `collect_metrics.py` 用 `rglob("*.jsonl")` 收 run，**一次 run 被记成四次**。
   一个 Harbor trial 目录下会落下四份 .jsonl：`workx-stream.jsonl`（真轨迹）、
   `workx-audit.jsonl`（审计日志）、`<task>.stream.raw.jsonl` 与 `<task>.session.jsonl`
   （导出器产出的**同一条轨迹的副本**，后者还是另一套 schema）。
   后三份解析不出任何步骤，等于凭空灌进一批「没跑验证命令」的样本。
2. `export_run.py` 遇到 0 字节 stream 在 `lines[-1]` 处 IndexError 崩掉整个导出。
   现在改成写一份说明性轨迹并**不生成会话**（伪造空会话会让人误读成「agent 什么都没做」）。

> ⚠️ 第 1 条是**分三次才修干净的**，值得单独记一笔：只排 `audit` 之前是 **38 个 run**
> （执行率假报 2.6%）；只排 `audit` 之后变成 **53 个 run**（4.4%）——导出器那份
> `*.stream.raw.jsonl` / `*.session.jsonl` 又混进来了；三类全排之后才是 **19 个 run / 6.7%**。
> 教训：**目录递归收 run 必须严格收口，否则分母失真，判定口径形同虚设**。

### Step 3 — 建 trace 分析回路（持续迭代）

审计日志已有骨架，补一份 **run manifest**（每轮：工具名/参数/结果摘要/token/耗时/错误类型），然后按 LangChain 的做法：
拉取失败 run → 并行 spawn 错误分析 agent → 汇总成 harness 改动 → 复测。
**改完必须复跑，且每次改动都要看有没有在别的题上回归**（防过拟合到单题）。

> ✅ **这条回路已手工跑通第一次，产出见 §2.5**。做法可直接当自动化模板：
> 1. 逐题读「最终答复」+ verifier 错误行 → 分出「被终止」还是「做错了」（`build/linux/diag_reason.py`）
> 2. 把 reward 与行为指标（步数 / 耗时 / 工具数）关联，看失败是不是「做得少」（`build/linux/analyze_fail.py`）
> 3. 对「被终止」的题**必须查日志** —— 只看出轨迹/命令会漏判：
>    stall 触发的是 Read/Edit 类调用，而 `commands` 只记 Bash
> 4. 归因后开针对性 issue（本次 → **#133**），修完复跑验证
>
> ⚠️ **这一步的价值远大于分数本身**：45% 这个数字什么都改不了，
> 但「9/19 的终止路径都经过一个失效的评审器」是一个可以立刻修、且能验证的具体缺陷。

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
