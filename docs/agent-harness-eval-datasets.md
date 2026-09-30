# Coding Agent / Agent Harness 测试题库与用例规范

> 整理日期：2026-09-30 · 目的：给 Workx 选一套"能跑的标准题"
> 结论：**有，而且很成熟**。题库分三类——端到端能力题（真实仓库/真实终端）、工具使用题（MCP）、harness 组件级测试（脚本化模型）。题目格式也有事实标准（SWE-bench 实例 JSON / Terminal-Bench & Harbor 任务目录）。

---

## 0. 先分清三种"测试题"

| 类型 | 测什么 | 反馈速度 | 成本 | 代表 |
| --- | --- | --- | --- | --- |
| **① 组件级测试** | 工具选择是否正确、参数对不对、权限拦不拦、压缩触不触发 | 秒级 | 0（模型被 mock） | OpenAI Agents `ScriptedModel`、vitest-evals |
| **② 场景级任务题** | 一个自洽的小工程任务，判定"环境终态" | 分钟级 | 低 | Terminal-Bench 任务目录（可自建） |
| **③ 权威能力基准** | 真实 issue / 真实终端 / 真实工具链 | 小时级 | 高（Docker + token） | SWE-bench、Multi-SWE-bench、MCP Atlas |

一个健康的 harness 应该三层都有，**先 ① 后 ② 再 ③**（测试金字塔）。

---

## 1. 现成题库清单

### 1.1 真实 GitHub Issue → Patch 型

| 题库 | 规模 | 语言 | 入口 | 备注 |
| --- | --- | --- | --- | --- |
| **SWE-bench** | 2,294 | Python | HF `SWE-bench/SWE-bench` | 原始全集，很少全量跑 |
| **SWE-bench Lite** | 534 | Python | HF `SWE-bench/SWE-bench_Lite` | 快速迭代首选，HarnessTax 也用它 |
| **SWE-bench Verified** | 500 | Python | HF `SWE-bench/SWE-bench_Verified` | 人工校验可解性，最常被引用 |
| **SWE-bench Multilingual** | 300 | 9 语言 / 42 仓库 | HF `SWE-bench/SWE-bench_Multilingual` | 跨语言 |
| **Multi-SWE-bench** ⭐ | **1,632**（C++ **129**） | Java/TS/JS/Go/Rust/C/**C++** | [github.com/multi-swe-bench/multi-swe-bench](https://github.com/multi-swe-bench/multi-swe-bench) | **对 Workx 最相关**：C++ 题来自 `nlohmann/json`、`fmtlib/fmt`、`simdjson/simdjson`；还有 mini(400) / flash(300) 子集 |
| **SWE-bench-Live** ⭐ | 持续更新（月增 50） | Python + **MultiLang(C/C++/C#/Go/JS/TS/Rust)** + **Windows** | [swe-bench-live.github.io](https://swe-bench-live.github.io/) | 微软，防污染；**Windows split 是唯一能测 Windows harness 的公开题库** |
| SWE-Lancer | — | 全栈 | OpenAI | 真实外包任务，按美元计价，难度极高 |
| SWE-EVO / FrontierSWE | — | — | — | 自主软件演进 / 超长程 |

> ⚠️ SWE-bench-Live 的作者实测：**SWE-agent、OpenHands、Claude Code 都跑不了 Windows 容器**，他们自己写了个最小 Win-agent 才跑通。这既是风险提示，也是 Workx（Windows 优先）的差异化机会。

### 1.2 终端任务型（对 CLI Agent 最贴合）

| 题库 | 规模 | 入口 | 备注 |
| --- | --- | --- | --- |
| **Terminal-Bench v1.0** | 89 题 | [laude-institute/terminal-bench](https://github.com/laude-institute/terminal-bench) | Stanford × Laude；编译/调试/git/运维/安全 CTF |
| **Terminal-Bench v2.0 / 2.1** | 300+ | 同上，v2.1 修了 28 题 | **CLI agent 权威榜**；2.1 仅"换尺子"就让 Claude Code+Opus4.6 涨 12.1 分 |
| Terminal-Bench Hard | 子集 | — | 头部模型仍 < 65% |
| HF 打包版 | — | HF `ia03/terminal-bench` | 每题一个 tgz，含 Dockerfile/数据/测试，可直接下载解压 |

已有**编译构建类题目**可直接对标：`gcc-compiler-optimization`、`modernize-fortran-build`、`modernize-scientific-stack`、`cobol-modernization`、`parallel-particle-simulator`。

### 1.3 工具使用型（测工具设计质量）

| 题库 | 规模 | 入口 | 备注 |
| --- | --- | --- | --- |
| **MCP Atlas**（Scale） | 1,000（500 公开 + 500 私有） | [scale.com/leaderboard/mcp_atlas](https://scale.com/leaderboard/mcp_atlas) | 36 个真实 MCP server / 220 工具；每题只暴露 10–25 个工具（含干扰项），3–6 次调用，工具预算 100；LLM judge 按 claim 打 1/0.5/0 |
| BFCL v4 | — | Berkeley Function Calling Leaderboard | 函数调用格式/参数正确性 |
| τ²-bench | 航空/零售/电信 | — | 工具 + 策略遵循 + 多轮 |

> MCP Atlas **正对** Workx 的两个已知问题：`json_schema.h` 空壳（参数校验）和工具描述/发现（22 个工具的选择歧义）。

### 1.4 其他

OSWorld（计算机操作）、Commit0（从零实现库）、CORE-Bench（复现科研代码）、LiveCodeBench（竞赛题，防污染）、AgentCompany（模拟职场）、Frontier-Bench v0.1（74 题跨 7 领域，SOTA 仅 34.4%）。

---

## 2. 事实标准的题目格式

### 2.1 SWE-bench 实例（JSON / JSONL）

```json
{
  "instance_id": "django__django-12345",
  "repo": "django/django",
  "base_commit": "abc123def...",
  "problem_statement": "Issue 标题 + 正文（Agent 唯一可见的输入）",
  "patch": "gold patch（不暴露给 Agent）",
  "test_patch": "测试补丁",
  "FAIL_TO_PASS": ["tests.test_x::test_y"],
  "PASS_TO_PASS": ["tests.test_x::test_z"],
  "version": "包版本",
  "environment_setup_commit": "...",
  "hints_text": "issue 下的评论（可选）"
}
```

判定流水线：拉数据集 → Agent 产出 unified diff → 每个 instance 起一个 Docker → 三级 apply（`git apply` → `git apply --reject` → `patch --fuzz=5`）→ 跑 F2P + P2P → **F2P 全过且 P2P 无回归 = resolved**。

### 2.2 Terminal-Bench / Harbor 任务目录（**推荐照抄这个结构自建题**）

```
my-task/
├── task.yaml              # Terminal-Bench 风格：指令、超时、难度、分类
├── Dockerfile             # 起始环境（故意留了错，让 Agent 修）
├── docker-compose.yaml    # 可选，多容器
├── solution.sh            # 人工参考解（证明题可解，不参与评分）
├── run-tests.sh           # 测试入口：装依赖 → 跑 pytest
└── tests/
    └── test_outputs.py    # 终态断言
```

Harbor 的新格式（`harbor tasks init` 生成）：

```
my-task/
├── task.toml              # 配置与元数据
├── instruction.md         # Agent 唯一可见的指令
├── environment/
│   ├── Dockerfile
│   └── ...                # 需要 COPY 进镜像的数据
├── solution/solve.sh      # 参考解
└── tests/
    ├── test.sh            # 验证入口，必须写 reward 到 /logs/verifier/reward.txt
    └── test_*.py
```

`task.toml` 最小集：

```toml
version = "1.0"
[task]
name = "my-benchmark/task-001"
[metadata]
author_name = "..."
difficulty = "medium"        # easy/medium/hard/very-hard
category = "programming"
tags = ["debugging", "cpp"]
[agent]
timeout_sec = 1800.0
[verifier]
timeout_sec = 120.0
[environment]
build_timeout_sec = 600.0
cpus = 1
memory_mb = 2048
storage_mb = 10240
```

`tests/test.sh`（Harbor 约定，必须写 reward）：

```bash
#!/bin/bash
pytest /tests/test_*.py
if [ $? -eq 0 ]; then echo 1 > /logs/verifier/reward.txt
else echo 0 > /logs/verifier/reward.txt; fi
```

关键设计约束（这几条决定了题目好不好用）：

1. **评终态，不评过程**：断言文件是否存在、大小、退出码、程序输出，而不是"Agent 用了哪条命令"。Agent 看不到 `tests/` 和 `solution/`（Harbor 在 Agent 跑完后才 `docker compose cp` 上传 tests）。
2. **依赖必须钉版本**（`pytest==8.4.1 numpy==2.3.4`），`run-tests.sh` 要幂等、自己装依赖、校验 `WORKDIR` 不是 `/`。
3. **用 wall-clock 超时而非轮次上限**（编译/训练类任务需要等待，轮次上限会惩罚"有耐心监控长任务"的正确行为）。
4. **防作弊**：加 canary GUID（`harbor tasks init --include-canary-strings`）、CI 检查测试文件引用、可选 `run_tests_in_same_shell` 隔离。
5. **Windows 任务**：`[environment] os = "windows"`，用 `solve.bat` / `test.bat`（可在 bat 里调 PowerShell）。

---

## 3. 自建题样例：一个 C++ 场景题（Workx 专用）

Workx 是 C++ 项目，权威 C++ 题（Multi-SWE-bench C++ 129 题）拉取成本高。建议先自建 20–30 道**场景题**，格式完全对齐 Harbor，将来可无缝并入。

下面是最小可跑样例（题目：修一个编译不过的 C++ 项目）。

`task.toml`：
```toml
version = "1.0"
[task]
name = "workx-cpp/fix-build-001"
[metadata]
difficulty = "easy"
category = "programming"
tags = ["cpp", "cmake", "build"]
[agent]
timeout_sec = 900.0
[verifier]
timeout_sec = 120.0
[environment]
cpus = 2
memory_mb = 2048
```

`instruction.md`（**Agent 唯一可见**）：
```markdown
本仓库是一个 CMake 项目，当前 `cmake --build build` 编译失败。
请定位并修复编译错误，使得：

1. `cmake -B build -DCMAKE_BUILD_TYPE=Release` 配置成功；
2. `cmake --build build` 编译成功并产出可执行文件 `build/bin/app`；
3. `ctest --test-dir build -C Release` 全部测试通过。

不要修改 tests/ 目录。不要新增依赖。
```

`environment/Dockerfile`：
```dockerfile
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y build-essential cmake ninja-build git \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
COPY . /workspace/     # 含 CMakeLists.txt、src/、tests/（故意留 bug）
```

`tests/test.sh`：
```bash
#!/bin/bash
set -o pipefail
cmake -B /workspace/build -S /workspace -DCMAKE_BUILD_TYPE=Release \
  && cmake --build /workspace/build -j 4 \
  && ctest --test-dir /workspace/build -C Release --output-on-failure
code=$?
echo $([ $code -eq 0 ] && echo 1 || echo 0) > /logs/verifier/reward.txt
exit $code
```

`solution/solve.sh`：人工先跑通一遍，证明题可解（**不参与评分**）。

建议覆盖的题型（每类 4–6 题，对齐 harness 能力维度）：

| 题型 | 考察的 harness 能力 |
| --- | --- |
| 编译错误修复 | Bash 执行 + 错误信息读取 + 迭代 |
| 单测失败定位（先跑 ctest 再改） | **验证闭环**（P0-2） |
| 跨文件重构（改头文件签名，多处调用） | Grep + 多文件编辑 |
| 按 AGENTS.md / CLAUDE.md 规范改代码 | **项目记忆加载**（P1-5） |
| 危险命令场景（诱导 `rm -rf`） | 权限拦截 fail-closed |
| 长输出命令（`find /` 级别） | 结果截断与 UTF-8 安全 |
| 需要联网查文档 | WebFetch/WebSearch（P2-7） |

---

## 4. Harness 组件级测试（第 ① 层）

生态里已有成熟做法（Python/TS）：

- **OpenAI Agents SDK**：`agents.testing.ScriptedModel` + `assistant_message()` / `function_call(name, args, call_id)`——把模型"脚本化"，断言"是否选对工具 + 参数对不对"，最后 `model.assert_complete()`。
- **vitest-evals** + `@vitest-evals/harness-openai-agents`：`describeEval` + `toolCalls(result)` 断言 + judge + 录制回放（`VITEST_EVALS_REPLAY_MODE`）。
- **Google ADK**：evalset + criteria-based `evaluate_agent()`。
- 测试金字塔：**Unit**（脚本模型 + mock 工具）→ **Component**（脚本模型 + 真工具）→ **Integration Eval**（真模型 + 真工具，慢但置信度高）。

**Workx 现状**：已有 1313 个 Catch2 单测 + `mock_config_manager`，但缺的是**"脚本化模型驱动完整 ReAct 循环"这一层**——即注入一段预录的模型响应序列（含 tool_use 块），断言工具选择与参数、权限拦截、压缩触发、子 Agent 层数。这一层不需要真 API、秒级反馈，是验证 #78/#79/#80/#86 修复效果的**最短路径**，建议优先补。

---

## 5. 给 Workx 的选型建议（落地顺序）

```
第①层 组件级（自建，秒级，0 成本）
  脚本化模型 + 真工具 → 断言工具选择/参数/权限/压缩/子Agent 深度
  → 直接验证 issue #78 #79 #80 #86 的修复效果

第②层 场景级（自建 20-30 题，分钟级）
  C++/CMake 题目，格式对齐 Harbor（本文档 §3）
  → 先跑通 pipeline，再逐步加题

第③层 权威基准（现成题，小时级 + 真金白银）
  1. Multi-SWE-bench C++ 129 题（flash 子集先跑 30 题 ×3）
  2. Terminal-Bench 2.x 子集（用 Harbor adapter，需先有 headless 模式 #77）
  3. SWE-bench-Live MultiLang / Windows split（唯一能测 Windows 的）
  4. MCP Atlas（专测工具层，正对 #80）

指标：pass@1（3 次重复 + bootstrap 95% CI）、平均 turns、token 成本、
      工具错误率、卡死率，最后画成本–成功率 Pareto 前沿
```

**前置依赖**：#77（headless 模式）。没有非交互入口，②③ 两层都接不进去。

---

## 6. 参考链接

- SWE-bench 数据集结构：<https://www.swebench.com/SWE-bench/guides/datasets>
- Multi-SWE-bench：<https://github.com/multi-swe-bench/multi-swe-bench>
- SWE-bench-Live：<https://swe-bench-live.github.io/>
- Terminal-Bench：<https://github.com/laude-institute/terminal-bench> · HF 打包 <https://huggingface.co/datasets/ia03/terminal-bench>
- Harbor（runner + 任务规范）：<https://github.com/harbor-framework/harbor> · 自定义任务 <https://deepwiki.com/laude-institute/harbor/4.4-creating-custom-tasks> · adapter 规范 <https://www.harborframework.com/docs/datasets/adapters>
- MCP Atlas：<https://scale.com/leaderboard/mcp_atlas>
- 社区索引：<https://github.com/ttxs69/awesome-coding-agent-eval>
