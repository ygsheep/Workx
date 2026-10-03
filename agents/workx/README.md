# Workx × Harbor 适配器

把 Workx 的 headless 模式接进 [Harbor](https://github.com/harbor-framework/harbor)，
跑 **Terminal-Bench 2.0** 拿真实基准分。

对应 `docs/agent-harness-assessment.md` §3 Step 2，并为 §3 Step 2b（Issue #117）产出可采集的 run 产物。

- 适配器本体：`__init__.py`（`WorkxAgent(BaseInstalledAgent)`）
- 指标采集器：`scripts/harness/collect_metrics.py`

> ✅ **smoke 已跑通**：`regex-log` 一题 reward = 1.0（agent 11 iterations / 10 tool calls / 71s），
> 三份产物（stream / log / audit）均已取回，#121 的日志修复确认生效。
>
> ✅ **首轮 20 题已跑完**（2026-10-03，DeepSeek `deepseek-v4-flash`，`-k 1 -n 3`，墙钟 1h49m）：
> **pass@1 = 45.0%（9 / 20）**。同时采到 #117 行为统计：**验证命令执行率 6.7%（1/15），未达 ≥90%**。
> 完整口径、题单与三个必须交代的偏差见 `docs/agent-harness-assessment.md` §3 Step 2c。
> ⚠️ 不是随机样本（按容量排除 16 道超时 >30min 的题后取字母序前 20）、`-k 1` 无置信区间、
> 无对照组 —— **只能当量级确认，不能当结论**。放量前请先读「已知阻塞」。

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

## 3. 跑分

### 3.1 先把题库拉到本地（一次）

`harbor run -d terminal-bench@2.0` 每次都要去 registry 拉题，慢且容易卡。
先整包下载一次，之后一律用 `-p` 走本地路径：

```bash
harbor dataset download terminal-bench@2.0 -o .cache/tb2
```

> ⚠️ 这一步实测要 **十几分钟**，很容易被误判成卡死。实测一次 15 分钟 timeout 杀掉后
> 发现 34 MB / 77 题其实**已经下完了**（含 instruction.md / task.toml / tests / environment）。
> 超时候先 `ls .cache/tb2/terminal-bench | wc -l` 看看，别急着重下。

### 3.2 选题规则（可复现，不按难度挑）

Terminal-Bench 2.0 里有 16 题的 `[agent] timeout_sec > 1800`，最夸张的
`build-pov-ray` 是 **12000 秒（3.3 小时）**、`sam-cell-seg` 7200 秒、`compile-compcert` 2400 秒。
单机跑分扛不住，按**机器容量**排除这 16 题，在剩下的 61 题里按字母序取前 N。

> 这是**容量排除，不是难度挑选** —— 报分数时必须交代，否则等于挑软柿子。

### 3.3 并发数：看内存而不是 CPU

宿主实测 **12 CPU / 8 GB 内存**（Docker Desktop）。题目普遍声明 `cpus=1 / mem=2G`，
`-n 4` 峰值会到 8～11 GB，有 OOM 风险 → **用 `-n 3`**。

### 3.4 一条命令

```bash
export MSYS_NO_PATHCONV=1
export PYTHONPATH="D:/develop/Workspace/workx"      # ⚠️ 必须 Windows 形式，$PWD 是 POSIX 路径
export WORKX_API_KEY=... WORKX_BASE_URL=https://api.deepseek.com
export WORKX_MODEL=deepseek-v4-flash
export WORKX_AGENT_BINARY_PATH=/opt/workx-bin/workx-linux-amd64

harbor run \
  -p "D:/develop/Workspace/workx/.cache/tb2/terminal-bench" \
  -a agents.workx:WorkxAgent -m deepseek-v4-flash \
  -i <题名> ...                       # 按 3.2 的规则列出
  -k 1 -n 3 --force-build -y \
  --artifact /tmp/workx-stream.jsonl \
  --artifact /tmp/workx-run.log \
  --artifact /tmp/workx-audit.jsonl \
  --mounts '[{"type":"bind","source":"D:/develop/Workspace/workx/build/linux","target":"/opt/workx-bin","read_only":true}]'
```

跑完导出轨迹：

```bash
python scripts/harness/export_run.py --runs-dir jobs/<job-id> \
       --model deepseek-v4-flash --copy-raw --to-workx-projects
```

对齐 HarnessTax 口径（任务先平均、再 bootstrap 10k 取 95% CI）时把 `-k 1` 换成 `-k 3`。

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
| `WORKX_AGENT_BINARY_URL` | 空 | 预构建 `workx` 二进制 URL。**给了就跳过源码构建** |
| `WORKX_AGENT_BINARY_PATH` | 空 | 预构建二进制的**容器内**路径（配合 `harbor run --mounts` 挂卷）。 |
|  |  | 与 URL 二选一，URL 优先。用它是因为宿主 `python -m http.server` 会被回收， |
|  |  | 容器里 `curl` 直接 exit 7 —— **挂卷不依赖常驻服务** |
| `WORKX_AGENT_REPO_URL` | `https://github.com/ygsheep/Workx.git` | 源码构建回退路径用 |
| `WORKX_AGENT_REF` | `develop` | 构建分支 |
| `WORKX_AGENT_CWD` | 空（沿用 Harbor 默认） | 强制 workx 的工作目录，任务文件不在默认 cwd 时必填 |
| `WORKX_AGENT_PERMISSION_MODE` | `bypass-permissions` | 无人值守档 |
| `WORKX_AGENT_MAX_ITERATIONS` | 空（配置默认 40） | 复杂题可上调 |
| `WORKX_GOAL` | 空 | #126：显式声明验证目标（`tests_pass` / `cmd:<命令>` / `file_exists:<path>` …）。 |
|  |  | **评测必读**：工作目录没有 `CMakeLists.txt` / `package.json` 之类的项目标记时， |
|  |  | 目标探测返回 None、#78 门禁完全不介入 —— 这类题只能靠它显式指定 |
| `WORKX_VERIFY_BEFORE_FINISH` | headless 下 `true` | #126：门禁开关，做「开/关对照跑」时用 |
| `WORKX_VERIFY_MAX_ATTEMPTS` | `3` | #126：验证失败回灌上限 |

所有 `WORKX_*` 变量都会透传进容器；缺 `WORKX_API_KEY` 时 `run()` 会直接抛错而不是静默空跑。

### 预构建快路径（推荐）

源码构建要在容器里 bootstrap vcpkg + 全量编译，每题都来一遍代价极高。先构建一次：

**基线必须压到 glibc 2.35**（= ubuntu 22.04）：terminal-bench@2.0 里大量题目跑在
`python:3.13-slim-bookworm`（glibc 2.36）这类镜像上，24.04 产物要 `GLIBC_2.38`，一上去就
`GLIBC_2.38 not found`。而在 22.04 上构建又要 GCC 13（代码用了 `<format>`），
所以走 ubuntu-toolchain-r PPA。（实测：24.04 产物在 bookworm 上同时报
`GLIBC_2.38` 与 `GLIBCXX_3.4.32` 缺失，换成下面的配方后 `--version` 正常退出。）

```bash
export MSYS_NO_PATHCONV=1
docker run --rm \
  -v "$PWD:/src" \
  -v "$PWD/build/vcpkg_installed/x64-windows/include/nlohmann:/opt/nlohmann" \
  -v "$PWD:/out" -w /src ubuntu:22.04 bash -c '
  apt-get update -qq
  # ① gpg-agent 必须显式装：add-apt-repository 导入 PPA 签名时会 fork 它，
  #    只装 software-properties-common 会报 "probably not installed" 并整个失败
  apt-get install -y --no-install-recommends software-properties-common ca-certificates \
    gnupg gpg-agent
  add-apt-repository -y ppa:ubuntu-toolchain-r/test
  apt-get update -qq
  apt-get install -y --no-install-recommends build-essential cmake ninja-build g++-13 \
    pkg-config libcurl4-openssl-dev nlohmann-json3-dev binutils
  # ② apt 的 nlohmann 是 3.11.3，编不过 json.value(key, std::optional<T>)，
  #    必须换成 vcpkg baseline 的 3.12.0（直接复用本机已有的那一份头文件）
  rm -rf /usr/include/nlohmann && cp -r /opt/nlohmann /usr/include/nlohmann
  # ③ Ubuntu 的 libcurl-dev 不提供 CMake package config，而 CMakeLists 用的是
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
    -DCMAKE_C_COMPILER=gcc-13 -DCMAKE_CXX_COMPILER=g++-13 \
    -DCURL_DIR=/usr/local/lib/cmake/CURL \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
    -DWORKX_BUILD_TESTS=OFF -DWORKX_BUILD_EXAMPLES=OFF -DWORKX_BUILD_CONSUMER=OFF \
    -DWORKX_WITH_TREE_SITTER=OFF -DWORKX_FETCH_GRAMMARS=OFF
  cmake --build /tmp/wxbuild --target workx -j "$(nproc)"
  install -m 0755 /tmp/wxbuild/bin/workx /out/workx-linux-amd64
'
```

约 4 分钟，产物 10 MB。两个关键点：

- `-static-libstdc++ -static-libgcc`：消掉 `GLIBCXX_3.4.32` 依赖
  （GCC 13 的 libstdc++，jammy 只到 3.4.30；不静态链接的话旧镜像照样起不来）。
  加完后 `NEEDED` 只剩 `libcurl.so.4 / libm.so.6 / libc.so.6 / ld-linux-x86-64.so.2`。
- 产物体检：`objdump -T workx-linux-amd64 | grep -oE "GLIBC(XX)?_[0-9.]+" | sort -uV | tail -1`
  应输出 `GLIBC_2.35`；出现 `GLIBC_2.38` 或任何 `GLIBCXX_` 说明配方没生效。

把产物挂给 Harbor（**推荐挂卷，不要起 HTTP 服务** —— 宿主 `python -m http.server`
的后台进程会被回收，容器里 `curl` 直接 exit 7）：

```bash
export WORKX_AGENT_BINARY_PATH=/opt/workx-bin/workx-linux-amd64
harbor run ... --mounts '[{"type":"bind","source":"<宿主产物目录>","target":"/opt/workx-bin","read_only":true}]'
```

> 上面刻意**不用 vcpkg 装依赖**：容器内访问 github.com 常常不通，bootstrap vcpkg 会直接失败；
> 而 nlohmann / curl 都可以用 apt + 上面两个补丁凑出来。

---

## 已知阻塞

1. ✅ ~~**headless 不写日志**~~ —— 已由 PR #123 修复并合并，#121 已关闭。
   现在 headless 会在提前 return 之前调用 `init_logging_and_audit(allow_default_file=false)`，
   `WORKX_LOG_FILE` / `WORKX_AUDIT_FILE` 生效（headless 侧不回落 `~/.workx/logs`，避免并发跑题互相覆盖）。

2. ✅ ~~**glibc 基线**~~ —— **已解（2026-10-03）**：预构建配方改成
   `ubuntu:22.04 + ubuntu-toolchain-r PPA 的 g++-13 + -static-libstdc++ -static-libgcc`，
   glibc 需求上限从 `GLIBC_2.38` 降到 **`GLIBC_2.35`**，`GLIBCXX_*` 依赖归零。
   实测在 `debian:bookworm-slim`（glibc 2.36）上：旧二进制报
   `GLIBC_2.38 not found` + `GLIBCXX_3.4.32 not found`，新二进制 `--version` 正常退出。
   → 覆盖 ubuntu 22.04 / debian 12 / ubuntu 24.04 的题。
   ⚠️ 仍跑不了 `debian:bullseye-slim`（glibc 2.31）—— 那需要把基线压到 20.04，暂时没做。
   ⚠️ 试过但**走不通**的路线：在 24.04 上用 `-U_GNU_SOURCE` 消除 `__isoc23_strtol`（能把
   glibc 需求降到 2.34），但 libstdc++ 依赖 `_GNU_SOURCE` 下的 `pthread_cond_clockwait`，
   直接编译失败 —— 别再试。

3. 🔴 **官方镜像拉取 403**：`alexgshaw/<task>:20251031` 在 daocloud 等加速器上返回 403
   （加速器只缓存 Docker Hub 官方 library 镜像）。必须 `--force-build` 走本地构建。

4. ⚠️ **容器内 github.com 不通**（未显式设代理时）：源码构建回退路径里的
   `git clone vcpkg` 会失败，所以这条路径目前**只在有代理的环境可用**。预构建快路径不受影响。

5. 🔴 **并发构建会把镜像构建拖过 600 秒上限**：题目声明的
   `[environment] build_timeout_sec` 普遍是 **600 秒**，而 `-n 3` 时三个构建互相抢资源，
   实测首批 3 题里 **2 题直接 `EnvironmentStartTimeoutError`**（首轮 20 题就是这么废掉的）。
   → 单独放宽构建超时、不要动 agent 超时：
   **`--environment-build-timeout-multiplier 4`**（600 → 2400 秒）。
   加上之后 20 题里只剩 1 题环境超时（`fix-code-vulnerability`）。

6. ⚠️ **Harbor 报「Docker daemon is not running」可能是误报**：它的 preflight 是
   `subprocess.run(["docker", "info"], timeout=10)`，Docker Desktop **冷启动超过 10 秒**就
   被判成没启动（实测热的时候 `docker info` 只要 2.4s）。先 `docker info` 预热再重试一次。

6. ⚠️ **`export PYTHONPATH=$PWD` 在 Git Bash 下不生效**：`$PWD` 是 POSIX 路径，
   Windows Python 不认，会报 `No module named 'agents'` → 必须写
   `export PYTHONPATH="D:/develop/Workspace/workx"`。

7. 🔴 **门禁在无项目线索的题上不介入（#126）**：`detect_default_goal()` 只认
   `CMakeLists.txt+CTestTestfile.cmake` / `Cargo.toml` / `go.mod` / `package.json` /
   `Makefile`，像 `regex-log` 这种 `/app` 空目录的题探测不到目标 → #78 门禁完全不生效
   （首轮 453 行日志里 `#78` 标记 0 次）。
   → 短期靠 `WORKX_GOAL` 按题面显式声明；修复前「门禁触发率」这一项**结构性偏低**，
     不要拿它直接判达标/不达标。
   → ✅ **PR #127 后这项不再静默**：20 题实测里有 7 个 trial 落了
     `#78 gate inactive (no goal under '/app'; set WORKX_GOAL/--goal)`，
     能区分「门禁没介入」和「门禁判定通过」了。

8. ⚠️ **agent 超时 → stream 变 0 字节，且 verifier 可能照样判过**：
   撞上 `AgentTimeoutError` 时容器被销毁，`tee` 出来的 `/tmp/workx-stream.jsonl` 取不回来。
   20 题里有 **4 个是 0 字节**，其中 3 个（cancel-async-tasks / extract-elf /
   break-filter-js-from-html）**reward 仍是 1.0** —— 活干完了，只是没在超时前收尾。
   → 采集器会把这 4 个从**验证类指标的分母**里剔除（`stream_empty`，不是「没跑验证命令」，
     是「不知道它跑了什么」）；导出器给它们写说明性轨迹且**不生成会话**。
   → 别把「reward=0 的题」直接当成「agent 做错了」，先查 stream 是不是空的。
