# Workx Agent Harness 能力评估与改进路线

> 初评 2026-09-29 · **复审 2026-09-30**（develop `b2d41ca`）· **三次更新 2026-09-30 18:55**（develop `9a2de19`）· 对象：`Workx v0.10.1`（`src/core` + `src/agent` + `src/tui`）
> 方法：**静态代码审计 + 业界 Harness 评测方法论对齐**。
> ⚠️ 本文的分数是**基于代码的预估分，不是跑分结果**。P0 五项现已全部落地（#78 为 P1 / P2 部分），headless 入口已通，**但仍未跑过任何基准**，真实分数必须用 §3 的方案实测。
> 📌 可视化评分卡：`docs/agent-harness-scorecard.html`
> 🔴 **二次更正 2026-09-30 13:20**：P0-2「零实现」的判定**不成立**，`#31/#32` 的目标验证栈已存在（详见 §2.2 P0-2）。
> 该更正使「验证闭环」维度由 48 上调至 **55**，加权总分由 68.5 → **69.9**。
> ⚠️ **那是记账更正，不是能力提升**——能力自始至终就在代码里，只是初评与复审都没看见，误记为 0。
> ✅ **三次更新 2026-09-30 18:39**：#78 的 P1 / P2 已落地（`9a2de19` / PR #98）——主循环自带 FinalAnswer 前验证门禁，headless 默认开启，13 条用例全绿。
> 「验证闭环」维度 **55 → 65**，加权总分 **69.9 → 71.9**。⚠️ **这一次是真实的能力提升**（与二次更正性质不同）：默认路径开始真的执行验证了。

---

## 0. 一句话结论

Workx 的**工具层和安全层**完成度相当高（22 个工具、并发执行、fail-closed 权限、密钥脱敏、四级缓存感知压缩、MCP、Hooks、Plan mode 双防线），在自研 harness 里属于上游水平；
**验证闭环（Verification Loop）此前几乎空白，现已补齐 P1 / P2**（主循环门禁 + headless 默认开启），**非交互执行模式也已打通**（#77）；
但**仍没有任何能力评测集**——而业界 2026 年的共识恰恰是：这三项才是拉开 harness 差距的地方（LangChain 仅靠补验证闭环 + 环境上下文，Terminal-Bench 2.0 从 52.8% → 66.5%，+13.7 分，模型一行没改）。

**加权总分：71.9 / 100**（初评 61.05 → 复审 68.5 → 二次更正 69.9 → **三次更新 71.9**；详见 §2）

**复审一句话**：P0 五项现已全部落地——前四项把「能不能测」这个前置问题解决了，**#78 的 P1 / P2 则让验证闭环第一次真正跑在默认路径上**（先覆盖评测入口 headless）。
初评「补齐 P0 后约 82 分」的预估仍偏乐观，实际 71.9 —— 因为「评测与迭代」维度的分数取决于**真的跑过分**，而这件事至今一次都没做。

> 🔴 **更正**：原判「#78 零实现」应改为「**#78 的能力大部分早已存在，缺的是接线与默认开启**」。`#31/#32` 留下的
> `goal_verdict.h` / `verdict.h`（`check_goal` / `guard_command` / `parse_goal`）/ `GoalGuardedAgent` 均已实现且有测试，
> 只是 `react_loop.cpp` 从不调用 `check_goal()`，默认 `agent.type` 为空 → 纯 ReAct → 零验证。
> ✅ **现已接线**（2026-09-30，PR #98）：`run_verification_gate()` 进入主循环（FinalAnswer 落地前），headless 默认开启，13 条 `[issue78]` 用例全绿。
> ⚠️ **剩余缺口**：P3（交互式默认开启）、P4（命令自适应探测）、VF-08（headless 下验证不阻塞 stdin）。

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

| # | 维度 | 权重 | 初评 | 复审 | **三次** | 加权 | 变动依据 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 工具设计与执行 | 20 | 72 | 82 | **82** | 16.4 | **#80**：`json_schema.{h,cpp}` 275 行实现，接入 `executor.h:223` 作 `validate_input` 后兜底；新工具漏写不再裸奔 |
| 2 | 上下文工程 | 20 | 70 | 70 | **70** | 14.0 | 未动（#82 环境上下文注入未做） |
| 3 | **验证闭环** | 20 | 40 | 55 | **65** | 13.0 | ✅ **#78 P1/P2 落地**（`9a2de19`）：门禁进主循环 + headless 默认开启 → **真实能力提升 +10**。仍欠 P3/P4/VF-08，故不给更高档 |
| 4 | 安全与权限 | 15 | 68 | 75 | **75** | 11.25 | **#81**：基线 commit + 人工回滚入口（只读安全网）；Win 沙箱（#84）、白名单档（#85）仍未修 |
| 5 | 编排与可靠性 | 10 | 62 | 72 | **72** | 7.2 | **#79**：`SubAgentBudget` 批量 + run 累计双护栏、atomic CAS 预约；成本爆炸风险从无限变为可配上限 |
| 6 | 可观测性与成本 | 10 | 75 | 78 | **78** | 7.8 | headless json 输出带 `usage`；收尾报告含 `git_diff_summary` |
| 7 | **评测与迭代** | 5 | 15 | 45 | **45** | 2.25 | **#77**：headless 已通（`-p` + 三种输出格式 + 语义化退出码），前置解锁；但**无 adapter、无 eval 集、未跑过分** |
| | **合计** | 100 | 61.05 | 68.5 | **71.9** | — | 二次更正 48→55 是记账更正；三次 55→65 是 #78 P1/P2 带来的真实提升 |

### 2.1 已经是长板的部分（别动，写进卖点）

- **工具执行七步漏斗 + 并发**：同一批 tool_calls `std::async` 并行，且用 `wait_for(100ms)` 轮询保证取消即时生效。
- **权限 fail-closed**：拿不到 event_bus → 拒绝；超时 → 拒绝；用户取消 → 拒绝。这是正确方向，很多 harness 是 fail-open。
- **四级缓存感知压缩**：0.5/0.6/0.8/0.9 水位 + SoftNotice/Snip/Compact/Force/Stuck 五种动作，还专门处理了 prompt 缓存失效——这个是认真设计的。
- **密钥扫描**：写入拦截 + 输出 `[REDACTED:x]` 脱敏，审计日志也脱敏。
- **MCP + Hooks 双全**：stdio/HTTP 传输、OAuth、子 Agent 独立作用域；8 种 hook 事件。
- **1313 个分层单元测试**（agent 57 / core 16 / tui 20 / island 9 / helpers 5 + integration + benchmarks），LLM 侧有 mock。

### 2.2 差距清单（按严重度）

#### P0 — 阻塞级：不修就没法测、也没法谈分数

> **复审状态：5 项中 4 项已合入 develop**（2026-09-30）。下表「状态」列标记实际落地情况。

| ID | 问题 | 状态 | 落地证据 / 残留问题 |
| --- | --- | --- | --- |
| **P0-1** | 没有非交互 / headless 执行模式 | ✅ **已修**（#77，PR #90） | `src/agent/headless/headless.{h,cpp}`；CLI `-p/--print` + `--output-format`（`main.cpp:89-166`）；退出码 0/1/2 ⚠️ **零测试覆盖** |
| **P0-2** | 无验证闭环门禁 | ✅ **P1 / P2 已落地**（#78，PR #98；issue 部分关闭） | ✅ **已接线**：`run_verification_gate()` 位于 `react_loop.cpp` 主循环（FinalAnswer 落地之前），验证未通过 → 回灌重试，达 `verify_max_attempts` 后带警告降级；headless 以 `enabled_by_default=true` 默认开启。13 条 `[issue78]` 用例全绿（含「回灌必须消耗预算」的死循环守卫）。<br>🔴 **原判「零实现」不成立**：`git grep -i "precompletion\|verification_gate" -- src` 零命中只能证明**没用 LangChain 的命名**——本仓等价能力叫 `goal_verdict.h`（`AgentGoal` / `GoalStatus`）、`verdict.h`（`check_goal` / `guard_command` / `parse_goal`）、`GoalGuardedAgent`（`agent_loop_adapters.cpp:33`）。<br>⚠️ **剩余缺口**：交互式仍默认关（P3）、命令自适应探测未做（P4）、内部评审器两条退出路径不经门禁、VF-08 未补 |
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
  install.sh     # 拷贝 workx 二进制 + vendor 工具
  run.sh         # 调用 workx -p "$TASK" --output-format json
```
先跑 `terminal-bench@2.0` 的 **30 题随机子集 × 3 次**（对齐 HarnessTax 口径：任务平均后 bootstrap 10k 次取 95% CI），再逐步扩到全量 89 题。
模型选择上注意：Workx 主打 DeepSeek/GLM/Kimi，跑 SWE-bench 这类英文 Python 题会吃亏，建议**同时报一个 Claude/GPT 对照组**，才能把"harness 贡献"和"模型贡献"分开。

### Step 3 — 建 trace 分析回路（持续迭代）

审计日志已有骨架，补一份 **run manifest**（每轮：工具名/参数/结果摘要/token/耗时/错误类型），然后按 LangChain 的做法：
拉取失败 run → 并行 spawn 错误分析 agent → 汇总成 harness 改动 → 复测。
**改完必须复跑，且每次改动都要看有没有在别的题上回归**（防过拟合到单题）。

### 参考分数线（预估，非实测）

| 阶段 | 预估 Terminal-Bench 2.x pass@1 |
| --- | --- |
| 现状（已具备跑分条件，尚未跑分） | — |
| 补完 P0（headless + 深度限制 + schema 校验 + git 快照 + **#78 验证门禁 P1/P2**） | 45–58% |
| 再补 P1（环境上下文注入 + 预算提示 + 沙箱） | 55–68% |

对照：LangChain 的 deepagents-cli 基线 52.8% → 优化后 66.5%（gpt-5.2-codex，同一模型）。Workx 在工具层更完整，但模型侧若用国产模型会有额外折让。

---

## 4. 改进优先级（ROI 排序）

```
P0-1 headless 模式          ← ✅ 已通（#77，PR #90）
P0-2 PreCompletionChecklist  ← ✅ P1/P2 已落地（#78，PR #98）
                              剩余：P3 交互式默认开启 / P4 命令自适应探测 / VF-08
P0-3 子 Agent 深度 + 预算上限 ← ✅ 已修（#79，PR #92）
P0-4 schema 校验器补实现      ← ✅ 已修（#80，PR #90）
P0-5 git checkpoint/回滚      ← ✅ 已修（#81，PR #93）
  ↓ ⬅ **当前卡在这里**：跑第一轮 Terminal-Bench 子集，拿真实基线（比继续补 P1 更优先）
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
