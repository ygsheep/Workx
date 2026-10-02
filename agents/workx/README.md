# Workx × Harbor 适配器

把 Workx 的 headless 模式接进 [Harbor](https://github.com/harbor-framework/harbor)，
跑 **Terminal-Bench 2.0** 拿真实基准分。

对应 `docs/agent-harness-assessment.md` §3 Step 2，并为 §3 Step 2b（Issue #117）产出可采集的 run 产物。

- 适配器本体：`__init__.py`（`WorkxAgent(BaseInstalledAgent)`）
- 指标采集器：`scripts/harness/collect_metrics.py`

> ⚠️ **本适配器尚未在真实容器里跑过** —— 编写机上 Docker 守护进程未运行、WSL 被安全策略拉黑。
> 首次使用**务必按下面的顺序先 smoke 再放量**，任何一步失败都请先看「已知阻塞」。

---

## 1. 前置条件

| 项 | 要求 | 检查 |
| --- | --- | --- |
| Docker | 守护进程**运行中**（CLI 装了不等于在跑） | `docker info` |
| uv | Harbor 推荐用 uv 装 | `curl -LsSf https://astral.sh/uv/install.sh \| sh` |
| Harbor | ≥ 2026-03（含 `BaseInstalledAgent` 新接口） | `uv tool install harbor && harbor --help` |
| 模型凭据 | `WORKX_API_KEY`（按需 `WORKX_BASE_URL` / `WORKX_MODEL`） | `echo $WORKX_API_KEY` |

Apple Silicon 需额外 `export DOCKER_DEFAULT_PLATFORM=linux/amd64`。

## 2. Smoke：先跑一道题

```bash
export WORKX_API_KEY=...
harbor run \
  --dataset terminal-bench@2.0 \
  -a agents.workx:WorkxAgent \
  --include-task-name git-init \
  -k 1
```

看三件事：

1. `install()` 有没有把 `workx` 装进去（容器内 `workx --version`）
2. `/agent/command-*/stdout.txt` 里有没有 NDJSON（每行一个 step）
3. `/verifier/reward.txt` 是不是 0 或 1

## 3. 首轮跑分：30 题 × 3 次

对齐 HarnessTax 口径（任务先平均、再 bootstrap 10k 取 95% CI）：

```bash
harbor run --dataset terminal-bench@2.0 -a agents.workx:WorkxAgent -m <model> \
           --n-concurrent 4 -k 3
```

> 模型对照建议：Workx 主打 DeepSeek / GLM / Kimi，跑英文 Python 题会吃亏。
> **同时报一个 Claude / GPT 对照组**，才能把「harness 贡献」和「模型贡献」分开。

## 4. 采集 #117 行为统计

```bash
python scripts/harness/collect_metrics.py \
  --runs-dir jobs/<job-id> -o metrics.json --md metrics.md
```

若验证命令执行率 **< 90%**，按 #117 的判定规则另开针对性 issue。

---

## 配置（环境变量 / `--agent-kwarg`）

| 变量 | 默认 | 说明 |
| --- | --- | --- |
| `WORKX_AGENT_BINARY_URL` | 空 | 预构建 `workx` 二进制 URL。**给了就跳过源码构建**（推荐，快很多） |
| `WORKX_AGENT_REPO_URL` | `https://github.com/ygsheep/Workx.git` | 源码构建回退路径用 |
| `WORKX_AGENT_REF` | `develop` | 构建分支 |
| `WORKX_AGENT_CWD` | 空（沿用 Harbor 默认） | 强制 workx 的工作目录，任务文件不在默认 cwd 时必填 |
| `WORKX_AGENT_PERMISSION_MODE` | `bypass-permissions` | 无人值守档 |
| `WORKX_AGENT_MAX_ITERATIONS` | 空（配置默认 40） | 复杂题可上调 |

所有 `WORKX_*` 变量都会透传进容器；缺 `WORKX_API_KEY` 时 `run()` 会直接抛错而不是静默空跑。

### 预构建快路径（推荐）

源码构建要在容器里 bootstrap vcpkg + 全量编译，每题都来一遍代价极高。先构建一次：

```bash
docker run --rm -v "$PWD:/src" -w /src ubuntu:24.04 bash -c '
  apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git curl unzip zip tar \
    libcurl4-openssl-dev python3
  export VCPKG_ROOT=/opt/vcpkg VCPKG_FORCE_SYSTEM_BINARIES=1
  git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
  "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
  cmake --build build --target workx -j "$(nproc)"
  cp build/bin/workx /src/workx-linux-amd64
'
```

然后把产物挂到任意可访问 URL（对象存储 / 本地 HTTP / CI artifact），设 `WORKX_AGENT_BINARY_URL` 即可。

---

## 已知阻塞

1. **headless 不写日志**（2026-10-03 实测，追踪 issue **#121**）
   `src/tui/main.cpp` 的日志初始化块（第 180-221 行）位于 headless 提前 return（第 167-170 行）**之后**，
   于是 `WORKX_LOG_FILE` 被静默忽略、stderr 也是 0 字节，`~/.workx/logs/workx.log` 与审计日志均无新增。
   → 后果：门禁的 `#78 verification ...` 标记全部丢失，采集器的**门禁触发率**与**误报率**两项采不到
   （其余三项走 stream-json，不受影响）。修复前这两项分母会被自动缩小并在报告里标注「不可外推」。

2. **本机跑不了**：Docker 守护进程未运行 + WSL 在安全策略黑名单里，需人工启动/解禁。

3. **首次 `install()` 成本高**：源码构建路径在容器里要跑 vcpkg bootstrap + 全量编译，
   建议直接用预构建二进制。
