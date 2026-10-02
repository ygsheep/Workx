# Workx × Harbor 适配器

把 Workx 的 headless 模式接进 [Harbor](https://github.com/harbor-framework/harbor)，
跑 **Terminal-Bench 2.0** 拿真实基准分。

对应 `docs/agent-harness-assessment.md` §3 Step 2，并为 §3 Step 2b（Issue #117）产出可采集的 run 产物。

- 适配器本体：`__init__.py`（`WorkxAgent(BaseInstalledAgent)`）
- 指标采集器：`scripts/harness/collect_metrics.py`

> ✅ **install 链路已实测**（2026-10-03，Docker Desktop 29.6.1）：
> `harbor run ... --install-only --force-build` 跑通 —— 容器内 `apt-get install` 成功、预构建
> 二进制下载成功、`workx --version` 退出 0。
> ⚠️ **但完整 run（agent 真的解题 + verifier 判分）还没跑过**，卡在缺少模型凭据。
> 首次使用**务必按下面的顺序先 smoke 再放量**，任何一步失败都请先看「已知阻塞」。

---

## 1. 前置条件

| 项 | 要求 | 检查 |
| --- | --- | --- |
| Docker | 守护进程**运行中**（CLI 装了不等于在跑） | `docker info` |
| uv | Harbor 推荐用 uv 装 | `curl -LsSf https://astral.sh/uv/install.sh \| sh` |
| Harbor | ≥ 2026-03（含 `BaseInstalledAgent` 新接口） | `harbor --help`（实测 0.23.0） |
| `PYTHONPATH` | 必须包含**仓库根**，否则 `-a agents.workx:WorkxAgent` 这个 import path 找不到 | `export PYTHONPATH=$PWD` |
| 模型凭据 | `WORKX_API_KEY`（按需 `WORKX_BASE_URL` / `WORKX_MODEL`） | `echo $WORKX_API_KEY` |

Apple Silicon 需额外 `export DOCKER_DEFAULT_PLATFORM=linux/amd64`。
Windows / Git Bash 需 `export MSYS_NO_PATHCONV=1`，否则 `/src` 这类参数会被 MSYS 转成 Windows 路径。

## 2. Smoke：先跑一道题

```bash
export MSYS_NO_PATHCONV=1
export PYTHONPATH=$PWD
export WORKX_API_KEY=...
export WORKX_BASE_URL=https://api.deepseek.com   # 尾部会自动补 /v1/chat/completions
export WORKX_AGENT_BINARY_URL=http://host.docker.internal:8899/workx-linux-amd64

harbor run \
  --dataset terminal-bench@2.0 \
  -a agents.workx:WorkxAgent \
  --include-task-name regex-log \
  -k 1 -n 1 --force-build -y
```

`--force-build` **不能省**：Terminal-Bench 的官方镜像 `alexgshaw/*:20251031` 在常见国内加速器上
会 403（加速器只缓存 Docker Hub 官方 library 镜像），只能拿任务自带的 `environment/Dockerfile`
在本地构建。

不想烧 token 只想验证安装时，加 `--install-only`（`install()` 不需要 API key）：

```bash
harbor run -d terminal-bench@2.0 -a agents.workx:WorkxAgent -i regex-log \
           -k 1 -n 1 --install-only --force-build -y
```

看三件事：

1. `install()` 有没有把 `workx` 装进去（容器内 `workx --version`）—— 见 `jobs/<job-id>/job.log`
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
export MSYS_NO_PATHCONV=1
docker run --rm \
  -v "$PWD:/src" \
  -v "$PWD/build/vcpkg_installed/x64-windows/include/nlohmann:/opt/nlohmann" \
  -v "$PWD:/out" -w /src ubuntu:24.04 bash -c '
  apt-get update -qq
  apt-get install -y --no-install-recommends build-essential cmake ninja-build \
    pkg-config libcurl4-openssl-dev nlohmann-json3-dev ca-certificates
  # ① apt 的 nlohmann 是 3.11.3，编不过 json.value(key, std::optional<T>)，
  #    必须换成 vcpkg baseline 的 3.12.0（直接复用本机已有的那一份头文件）
  rm -rf /usr/include/nlohmann && cp -r /opt/nlohmann /usr/include/nlohmann
  # ② Ubuntu 的 libcurl-dev 不提供 CMake package config，而 CMakeLists 用的是
  #    find_package(CURL CONFIG REQUIRED) —— 自己造一个最小的
  mkdir -p /usr/local/lib/cmake/CURL
  cat > /usr/local/lib/cmake/CURL/CURLConfig.cmake <<CFG
set(CURL_FOUND TRUE)
if(NOT TARGET CURL::libcurl)
  add_library(CURL::libcurl UNKNOWN IMPORTED)
  set_target_properties(CURL::libcurl PROPERTIES
    IMPORTED_LOCATION "/usr/lib/x86_64-linux-gnu/libcurl.so"
    INTERFACE_INCLUDE_DIRECTORIES "/usr/include")
endif()
CFG
  cmake -S /src -B /tmp/wxbuild -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCURL_DIR=/usr/local/lib/cmake/CURL \
    -DWORKX_BUILD_TESTS=OFF -DWORKX_BUILD_EXAMPLES=OFF -DWORKX_BUILD_CONSUMER=OFF \
    -DWORKX_WITH_TREE_SITTER=OFF -DWORKX_FETCH_GRAMMARS=OFF
  cmake --build /tmp/wxbuild --target workx -j "$(nproc)"
  install -m 0755 /tmp/wxbuild/bin/workx /out/workx-linux-amd64
'
```

约 3 分钟，产物 8 MB。然后把产物挂到任意**容器内可访问**的 URL：

```bash
cd <产物目录> && python -m http.server 8899 --bind 0.0.0.0
# 容器侧用 http://host.docker.internal:8899/workx-linux-amd64
```

> 上面刻意**不用 vcpkg 装依赖**：容器内访问 github.com 常常不通，bootstrap vcpkg 会直接失败；
> 而 nlohmann / curl 都可以用 apt + 上面两个补丁凑出来。

---

## 已知阻塞

1. ✅ ~~**headless 不写日志**~~ —— 已由 PR #123 修复并合并，#121 已关闭。
   现在 headless 会在提前 return 之前调用 `init_logging_and_audit(allow_default_file=false)`，
   `WORKX_LOG_FILE` / `WORKX_AUDIT_FILE` 生效（headless 侧不回落 `~/.workx/logs`，避免并发跑题互相覆盖）。

2. 🔴 **glibc 基线**：上面配方产出的二进制是 ubuntu 24.04（glibc 2.39 / libcurl 8.5）产物，
   要求 `GLIBC_2.38`，**跑不了** `python:3.13-slim-bookworm`（2.36）和 `debian:bullseye-slim`（2.31）
   的题 —— terminal-bench@2.0 的 89 题里有 **43 题**会直接 `GLIBC_2.38 not found`。
   退到 ubuntu 22.04 也不行：代码用了 `<format>`（std::format），**至少要 GCC 13**，
   而 jammy 默认只有 GCC 11/12。要覆盖全量子集，得先搞出 jammy + g++-13（toolchain PPA）或等价的低基线构建。
   → **smoke 只挑 `ubuntu:24.04` 的题就绕得过去**（如 `regex-log`）。

3. 🔴 **官方镜像拉取 403**：`alexgshaw/<task>:20251031` 在 daocloud 等加速器上返回 403
   （加速器只缓存 Docker Hub 官方 library 镜像）。必须 `--force-build` 走本地构建。

4. ⚠️ **容器内 github.com 不通**（未显式设代理时）：源码构建回退路径里的
   `git clone vcpkg` 会失败，所以这条路径目前**只在有代理的环境可用**。预构建快路径不受影响。
