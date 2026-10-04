# 跑分工具集（Issue #117）

> 一句话原则：**分数异常时先怀疑评测环境本身，别急着归因到 agent 或模型。**
> 本目录的多数脚本都是为了把这个验证做便宜——每一份都是从一次真实翻车里长出来的。

| 脚本 | 作用 | 对应的翻车 |
| --- | --- | --- |
| [`run-harbor-replay.sh`](run-harbor-replay.sh) | 复跑一轮 Harbor（含**四道跑前卡口**） | 拿旧二进制/坏模型跑满一小时才发现白跑 |
| [`build-linux-container.sh`](build-linux-container.sh) | 在 Docker 里重建 Linux 二进制的完整配方 | 构建配方没留过档；不同潜意识复用错了 IOC 配置 |
| [`check_binary_signature.py`](check_binary_signature.py) | 校验产物二进制是否含目标改动 | 用没编进 #133 的二进制得出「#133 有效」的假结论 |
| [`fix_dataset_lineendings.py`](fix_dataset_lineendings.py) | 数据集行尾守卫（CRLF → LF） | Windows `autocrlf=true` 让 checksum 校验题**必然失败** |
| [`trial_metrics.py`](trial_metrics.py) | trial 成绩去噪采集（区分「真失败」与「没被判定」） | 45% 这个数字里混着大量根本没跑过的 trial |
| [`collect_metrics.py`](collect_metrics.py) | 采集**行为统计指标**（验证命令执行率等五项），判断 #78 是否达标 | — |
| [`export_run.py`](export_run.py) | 把一整轮跑分导出成**可读轨迹 Markdown** + **可导入的 workx 原生会话** | — |

---

## 跑一轮 Harbor：`run-harbor-replay.sh`

唯一变量是**二进制**：用 `--config` 喂 JobConfig，datasets / agents / mounts / 并发数
全部与基准轮对齐。跑前四道卡口缺一不可：

```bash
MSYS_NO_PATHCONV=1 bash scripts/harness/run-harbor-replay.sh          # 全量 20 题
MSYS_NO_PATHCONV=1 bash scripts/harness/run-harbor-replay.sh smoke    # 3 题，约 20~36 分钟
```

| 卡口 | 检查什么 | 不过会发生什么 |
| --- | --- | --- |
| ① 二进制签名 | `WORKX_AGENT_TIMEOUT_SEC` 等特征串，及 `-g<hash>` | 跑的是旧代码，结论全假 |
| ② 模型连通 | 真打一次 `/chat/completions` | 整轮 trial 全 0 且看不出错在哪 |
| ③ 数据集行尾 | `.cache/tb2/terminal-bench` 是否 LF | checksum 题必挂、`*.sh` bad interpreter |
| ④ 模型名对齐 | JobConfig 的 `model_name` = `WORKX_MODEL` | 产物元信息与实际调用不符，无法追溯 |

前提：仓库根有 `.env.harbor`（`WORKX_API_KEY=...`，已被 gitignore，模板见
`.env.harbor.example`）。Windows/Git Bash 下三个必踩的坑已在脚本注释里写明：
`MSYS_NO_PATHCONV=1`、`PATH` 要含 `~/.local/bin`、`PYTHONPATH` 必须 `$(pwd -W)`。

## 重建 Linux 二进制：`build-linux-container.sh`

本机 WSL 被安全策略拉黑，只能在容器里编。配方对齐 CI 的 `build-test.yml`，
只编 `workx` 主程序。**两个关键约束**（详见脚本头部注释）：

1. 镜像 ≥ `ubuntu:24.04` —— `logger.h` 用了 `<format>`，22.04 的 GCC 11 没有这个头。
2. **构建目录必须是容器本地路径**（`/build_local`），挂 Windows bind mount 会让
   CMake 探测不到编译器。只有 vcpkg 目录适合挂出来复用。

产物落到 `build/linux/workx-linux-amd64`，旧版自动备份成 `.old-<日期>`，结尾自动跑
`check_binary_signature.py` 验签名。

## 去噪采集：`trial_metrics.py`

Harbor 的原始 `reward.txt` 只有 0/1，会把「verifier 压根没跑起来」也算成 agent 失败。
本脚本按轨迹把它拆成 `verifier` / `environment` / `unknown` / `ok` 四类，
并给出 `pass_rate_all`（须等于 Harbor 原始口径，用来自证采集无误）与 `pass_rate_valid`。

```bash
python scripts/harness/trial_metrics.py --self-test          # 自检，26+ 项
python scripts/harness/trial_metrics.py jobs/<job-id> --md out.md
```

## 数据集行尾：`fix_dataset_lineendings.py`

```bash
python scripts/harness/fix_dataset_lineendings.py --check    # 体检，非 0 退出
python scripts/harness/fix_dataset_lineendings.py --fix      # 就地 CRLF -> LF
```

含 NUL 的文件视为二进制，**绝不改写**（图片 / 压缩包里出现 `\r\n` 是正常的）。

## 二进制签名：`check_binary_signature.py`

```bash
python scripts/harness/check_binary_signature.py build/linux/workx-linux-amd64
python scripts/harness/check_binary_signature.py <bin> --only-version
```

⚠️ 新增特征串前**必须先在真实产物里 grep 到再写进 MARKS**：`WORKX_TIMEOUT` 明明写死在
`app_config.cpp` 的 env_var 里，产物中却搜不到（字符串表优化），列为必选项就会误报。

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

### ⚠️ `--runs-dir` 会遇到的重复计数（踩过三次）

一个 Harbor trial 目录下有**四份** .jsonl，只有第一份是 run：

| 文件 | 是 run 吗 | 说明 |
| --- | --- | --- |
| `workx-stream.jsonl` | ✅ | 真轨迹 |
| `workx-audit.jsonl` | ❌ | 审计日志，一条工具调用都没有 |
| `<task>.stream.raw.jsonl` | ❌ | `export_run.py --copy-raw` 拷来的**轨迹副本** |
| `<task>.session.jsonl` | ❌ | `export_run.py` 产的可导入会话，**另一套 schema** |

实测踩坑顺序（同一份 20 题产物）：38 个 run（2.6%）→ 只排 audit 后 53 个（4.4%）
→ 三类全排后 **19 个（6.7%）**。**目录递归收 run 必须严格收口**，否则分母失真。

自检里有 5 条 `is_stream_file()` 护栏盯着这个，放宽规则时必须同步改。

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
