# 行为统计采集器（Issue #117）

从 Workx headless 的 run 产物里采集 `docs/agent-harness-assessment.md` §3 Step 2b
定义的五项指标，用于验收 #78 的**行为统计口径**（验证命令执行率 ≥ 90%）。

| 指标 | 口径 | 证据源 |
| --- | --- | --- |
| 验证命令执行率 | 结束前执行过测试/构建命令的 run 占比（目标 ≥ 90%） | `stream.jsonl` |
| 门禁触发率 | 由 `#78 verification not passed` 回灌驱动的占比 | 日志文件 |
| 降级率 | 达 `verify_max_attempts` 后带警告终止的占比 | `stream.jsonl`（`goal_status == 3`）+ 日志 |
| 误报率 | 探测不到命令（`unavailable`）直接放行的占比 | 日志文件 |
| 成本增幅 | 门禁开 / 关的平均 turns / token 增量 | 两组 run 对照 |

## 用法

```bash
# 自检（用内置样例验证解析链路，不是跑分）
python scripts/harness/collect_metrics.py --self-test

# 扫一个 job 目录
python scripts/harness/collect_metrics.py --runs-dir jobs/<job-id> -o metrics.json --md metrics.md

# 带对照组算成本增幅
python scripts/harness/collect_metrics.py --runs-dir jobs/on --baseline jobs/off --md metrics.md
```

退出码：`0` 达标或无数据，`3` **验证命令执行率未达标**（可直接用于 CI 判定）。

## 产物来源

- `stream.jsonl`：`workx -p "<task>" --output-format stream-json` 的 stdout（NDJSON）
- `*.log`：`WORKX_LOG_FILE` 指向的日志文件

采集器按「同目录同 stem 的 `.log`」自动配对日志；找不到就记 `evidence=none`。

## 诚实性设计（重要）

**绝不把「采不到」当成「没触发」。** 缺日志的 run 会被从「门禁触发率 / 误报率」的
分母里剔除，并在 `evidence_coverage.log_missing` 与 Markdown 报告里显式告警。
看到分母缩小时不要外推到全量。

## fixtures/

`fixtures/` 下的 `run_a/b/c` 是**手工构造的样例**，用于 `--self-test` 验证解析链路，
**不是真实跑分数据**，不得用于任何分数声明。其字段结构与
`src/agent/headless/headless.cpp` 的 `step_json()` / `result_json()` 一致
（nlohmann 默认按 key 字母序输出）。
