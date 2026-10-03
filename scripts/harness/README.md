# 跑分工具集（Issue #117）

| 脚本 | 作用 |
| --- | --- |
| [`collect_metrics.py`](collect_metrics.py) | 采集**行为统计指标**（验证命令执行率等五项），判断 #78 是否达标 |
| [`export_run.py`](export_run.py) | 把一整轮跑分导出成**可读轨迹 Markdown** + **可导入的 workx 原生会话** |

---

## `collect_metrics.py` —— 行为统计采集器

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

---

## `export_run.py` —— 跑分产物导出器

Harbor 留下的 `artifacts/tmp/workx-stream.jsonl` 是**评测 schema**（step_number/type/…），
既不好读，也不是 `~/.workx/projects/` 下会话查看工具认的格式。这个脚本把它变成两份东西：

| 产物 | 内容 |
| --- | --- |
| `<task>.transcript.md` | 逐步展开 thought → action → observation，附工具序列、最终答复、verifier 输出 |
| `<task>.session.jsonl` | workx 原生会话格式，可直接放进 `<config_dir>/projects/<name>/` 让本地工具渲染 |

```bash
# 自检（内置样例，验证两套 schema 的转换不丢步、工具名不丢）
python scripts/harness/export_run.py --self-test

# 导出整个 job 到 jobs/<job-id>/export/
python scripts/harness/export_run.py --runs-dir jobs/<job-id> \
       --model deepseek-v4-flash --copy-raw

# 顺手装进 ~/.workx/projects/terminal-bench-<题名>/ 给本地会话工具看
python scripts/harness/export_run.py --runs-dir jobs/<job-id> --to-workx-projects
```

常用参数：`--project-prefix`（默认 `terminal-bench`）、`--taskset-root`（取任务原文的
Harbor 缓存根）、`--cwd`（写进会话的工作目录，默认 `/app`）、`--copy-raw`（连带复制原始
jsonl / 日志 / 审计）、`--config-dir`（默认 `~/.workx`）。

### 两个容易踩的坑

1. **工具名要从上一个 `action` 带到紧随的 `observation` 上** —— stream 的 observation 行
   本身不带 `tool_name`，漏了这条，会话里所有 tool 消息的 `toolName` 都是空的。
2. **给 python 的路径参数一律用 `C:/...` 形式** —— 设了 `MSYS_NO_PATHCONV=1` 之后
   `~/.cache/...` 会展开成 POSIX 路径，Windows Python 打不开。

### 目录约定

会话落盘位置必须与 `src/agent/session/session_store.cpp` 一致，否则本地工具扫不到：

```
<config_dir>/projects/<encode_project_path(cwd)>/<session_id>.jsonl
```

`encode_project_path()` 只把 `\ / :` 换成 `-`（`src/core/utils/path_encoder.cpp`）。
