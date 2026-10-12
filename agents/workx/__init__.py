"""Workx —— Harbor / Terminal-Bench 2.0 自定义 agent 适配器。

对应 docs/agent-harness-assessment.md §3 Step 2（写 Harbor adapter 并跑子集），
并为 §3 Step 2b（Issue #117 行为统计）产出可采集的 run 产物。

契约依据
    本文件按 Harbor **2026-03-24 重构后**的 `BaseInstalledAgent` 现行接口编写：
        install(environment) -> None                 （取代旧的 install-*.sh.j2 模板）
        @with_prompt_template run(instruction, environment, context) -> None
        populate_context_post_run(context) -> None   （可选）
    旧接口 `create_run_agent_commands` / `_install_agent_template_path` / `ExecInput`
    已在同次重构中移除，若 Harbor 版本较老需按 CHANGELOG 反向迁移。

用法
    harbor run \\
        --dataset terminal-bench@2.0 \\
        -a agents.workx:WorkxAgent \\
        -m <model> \\
        --n-concurrent 4

#132 验证目标注入（让 #78 门禁在无工程标记的容器里也能介入）
    宿主环境设置 WORKX_AGENT_GOAL 即可，适配器转成 --goal 命令行参数
    （优先级 > WORKX_GOAL env > config.json）：
        WORKX_AGENT_GOAL=tests_pass            测试全绿
        WORKX_AGENT_GOAL=build_clean           编译零 error
        WORKX_AGENT_GOAL="file_exists:/app/out.txt"
        WORKX_AGENT_GOAL="cmd:python3 -m pytest -q"
    不设置时不注入 —— 门禁行为与此前一致（无 goal 则结构性不介入）。
    ⚠️ #137 已实测门禁 fail-open 有下游效应（墙钟超时成为通过瓶颈），
    是否在跑分中默认开启需先看该 issue 的结论，故此处保持显式 opt-in。

⚠️ 未验证声明
    本适配器在**没有可用 Docker 守护进程**的机器上编写（WSL 被安全策略拉黑），
    `install()` / `run()` 的实际容器内行为**尚未实跑验证**。首次使用请先跑单题 smoke：
        harbor run -d terminal-bench@2.0 -a agents.workx:WorkxAgent \\
                   --include-task-name git-init -k 1

#166 方案 C 按题下发墙钟预算
    harbor 的 agent 超时是**按题**的（task.toml 的 `[agent] timeout_sec`），而 JobConfig
    的 `agents[].env` 是 **job 级**（一份值给所有题），表达不了"20 题各自 900/1200/1800s"。
    于是改由本适配器就地推导：trial 产物目录的 config.json 里带着 `task.path`
    （宿主侧题目目录），读它下面的 task.toml 即可拿到本题真实超时，再留 10% 余量
    下发 `WORKX_AGENT_TIMEOUT_SEC` —— 让 agent 在**被外层硬超时杀掉之前**主动收敛
    （#137 建议 3 / #166 方案 C）。宿主若已显式设置该变量则以宿主为准。
"""

from __future__ import annotations

import base64
import json
import os
import pathlib
import re
import shlex
from typing import Any

try:  # Python >= 3.11
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - 老解释器走正则兜底
    tomllib = None  # type: ignore[assignment]

from harbor.agents.installed.base import BaseInstalledAgent, with_prompt_template

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

DEFAULT_REPO_URL = "https://github.com/ygsheep/Workx.git"
DEFAULT_REF = "develop"

BIN_PATH = "/usr/local/bin/workx"
INSTRUCTION_PATH = "/tmp/workx-instruction.md"
STREAM_PATH = "/tmp/workx-stream.jsonl"
LOG_PATH = "/tmp/workx-run.log"
AUDIT_PATH = "/tmp/workx-audit.jsonl"

#: 运行时系统依赖（Terminal-Bench 任务镜像多为 Ubuntu / Debian）
#: 注意用 libcurl4（运行时）而非 libcurl4-openssl-dev：workx 链接 libcurl.so.4，
#: 头文件不是必需的；源码构建需要的 -dev 在 BUILD_PACKAGES 里单独装。
APT_PACKAGES = "ca-certificates curl git libcurl4 tzdata"

#: 源码构建所需的编译依赖
BUILD_PACKAGES = (
    "build-essential cmake ninja-build pkg-config unzip zip tar "
    "libcurl4-openssl-dev python3"
)


#: 按题墙钟预算的余量系数：留 10% 给「收尾 + 验证」。
#: agent 侧会在预算耗尽前触发收敛提示（wall_clock_reserve_sec），故预算必须**略小于**
#: 题目声明的 timeout_sec，否则收敛提示还没生效就被外层硬超时杀掉了。
WALL_CLOCK_MARGIN = 0.9


def _read_agent_timeout_sec(task_dir: str) -> float | None:
    """从题目目录的 task.toml 读 `[agent] timeout_sec`（读不到返回 None）。

    优先 tomllib（Python >= 3.11）；老解释器走一段**分节感知**的正则兜底，
    避免把 `[verifier] timeout_sec` / `[environment] build_timeout_sec` 误当成 agent 的。
    """
    try:
        text = (pathlib.Path(task_dir) / "task.toml").read_text(encoding="utf-8")
    except OSError:
        return None

    if tomllib is not None:
        try:
            data = tomllib.loads(text)
        except (tomllib.TOMLDecodeError, ValueError):
            return None
        value = (data.get("agent") or {}).get("timeout_sec")
        if isinstance(value, (int, float)) and value > 0:
            return float(value)
        return None

    section = ""
    for raw in text.splitlines():  # pragma: no cover - 仅老解释器
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line.strip("[]").strip()
            continue
        if section != "agent":
            continue
        match = re.match(r"timeout_sec\s*=\s*([0-9]+(?:\.[0-9]+)?)", line)
        if match:
            return float(match.group(1))
    return None


def _source_build_script(repo_url: str, ref: str) -> str:
    """容器内从源码构建 workx 的脚本（install() 的回退路径）。"""
    return f"""set -euo pipefail
git clone --depth 1 --branch {shlex.quote(ref)} {shlex.quote(repo_url)} /opt/workx-src
cd /opt/workx-src
export VCPKG_ROOT=/opt/vcpkg
export VCPKG_FORCE_SYSTEM_BINARIES=1
git clone --depth 1 https://github.com/microsoft/vcpkg.git "$VCPKG_ROOT"
"$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
cmake -S . -B build -G Ninja \\
    -DCMAKE_BUILD_TYPE=Release \\
    -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \\
    -DVCPKG_MANIFEST_INSTALL=ON
cmake --build build --target workx -j "$(nproc)"
install -m 0755 build/bin/workx {BIN_PATH}
{BIN_PATH} --version
"""


class WorkxAgent(BaseInstalledAgent):
    """把 Workx headless 接进 Harbor 的 installed agent。"""

    def __init__(self, **kwargs: Any) -> None:
        super().__init__(**kwargs)
        self.repo_url = os.environ.get("WORKX_AGENT_REPO_URL", DEFAULT_REPO_URL)
        self.ref = os.environ.get("WORKX_AGENT_REF", DEFAULT_REF)
        # 预构建二进制：给了 URL 就下载；给了容器内路径就用本地拷贝；两者都无才源码构建
        self.prebuilt_url = os.environ.get("WORKX_AGENT_BINARY_URL", "")
        # 容器内路径（配合 harbor run --mounts 把宿主目录挂进来）。
        # 比 URL 稳：不依赖宿主常驻 HTTP 服务，也不依赖容器出网。
        self.prebuilt_path = os.environ.get("WORKX_AGENT_BINARY_PATH", "")
        self.max_iterations = os.environ.get("WORKX_AGENT_MAX_ITERATIONS", "")
        self.cwd = os.environ.get("WORKX_AGENT_CWD", "")
        self.permission_mode = os.environ.get(
            "WORKX_AGENT_PERMISSION_MODE", "bypass-permissions"
        )
        # #132 建议动作 c：显式声明验证目标，填上「门禁 0%」的结构性空洞。
        # Terminal-Bench 的任务容器里没有 CMakeLists / package.json 之类的工程标记，
        # detect_default_goal() 返回 None → #78 门禁结构性不介入（首轮实测触发率恒 0）。
        # 这里支持两种注入方式：
        #   1. WORKX_AGENT_GOAL（本适配器读取，走 --goal 命令行，优先级最高）
        #   2. 宿主直接设 WORKX_GOAL（_workx_env 已按 WORKX_* 前缀透传进容器）
        # 取值格式见 src/agent/core/goal_verdict.h parse_goal：
        #   tests_pass / build_clean / lint_zero / file_exists:<path> / cmd:<command>
        self.goal = os.environ.get("WORKX_AGENT_GOAL", "")

    # ------------------------------------------------------------------
    # 元数据
    # ------------------------------------------------------------------

    @staticmethod
    def name() -> str:
        return "workx"

    def version(self) -> str:
        return "0.11.0"

    # ------------------------------------------------------------------
    # 安装
    # ------------------------------------------------------------------

    async def install(self, environment: Any) -> None:
        """容器内的两阶段安装：运行时系统依赖 → workx 本体。

        编译期依赖（cmake / build-essential …）**只在走源码构建时才装**：
        预构建快路径下它们毫无用处，却要把 install 阶段拖慢数分钟（每 trial 各装一次）。
        """
        await self.exec_as_root(
            environment,
            command=f"apt-get update -qq && apt-get install -y --no-install-recommends "
            f"{APT_PACKAGES}",
        )

        if self.prebuilt_url:
            await self.exec_as_root(
                environment,
                command=f"curl -fsSL {shlex.quote(self.prebuilt_url)} -o {BIN_PATH} "
                f"&& chmod 0755 {BIN_PATH} && {BIN_PATH} --version",
            )
        elif self.prebuilt_path:
            await self.exec_as_root(
                environment,
                command=f"install -m 0755 {shlex.quote(self.prebuilt_path)} {BIN_PATH} "
                f"&& {BIN_PATH} --version",
            )
        else:
            await self.exec_as_root(
                environment,
                command=f"apt-get install -y --no-install-recommends {BUILD_PACKAGES}",
            )
            await self.exec_as_root(
                environment, command=_source_build_script(self.repo_url, self.ref)
            )

    # ------------------------------------------------------------------
    # 执行
    # ------------------------------------------------------------------

    @with_prompt_template
    async def run(self, instruction: str, environment: Any, context: Any) -> None:
        """把题面交给 `workx -p`，产物落到 STREAM_PATH / LOG_PATH。

        题面经 base64 落盘再用 stdin 喂给 workx（`workx -p -` 读 stdin），
        避免引号、反引号、$ 等字符在 shell 里被吃掉。

        末尾显式 `exit 0`：agent 非零退出不该中断 trial —— **成败由 verifier 判定**，
        真实退出码已打印到 stderr 便于事后排查。
        """
        encoded = base64.b64encode(instruction.encode("utf-8")).decode("ascii")
        await self.exec_as_agent(
            environment,
            command=f"printf %s {shlex.quote(encoded)} | base64 -d > {INSTRUCTION_PATH}",
        )

        flags = f"--output-format stream-json --permission-mode {self.permission_mode}"
        if self.max_iterations:
            flags += f" --max-iterations {shlex.quote(self.max_iterations)}"
        if self.goal:
            # #132：显式验证目标（--goal 优先级 > WORKX_GOAL > config.json）
            flags += f" --goal {shlex.quote(self.goal)}"
        prefix = f"cd {shlex.quote(self.cwd)} && " if self.cwd else ""

        await self.exec_as_agent(
            environment,
            command=(
                f"{prefix}{BIN_PATH} -p - {flags} < {INSTRUCTION_PATH} | tee {STREAM_PATH}; "
                f'st=$?; echo "[workx] exit=$st" >&2; exit 0'
            ),
            env=self._workx_env(),
        )

    # ------------------------------------------------------------------
    # 辅助
    # ------------------------------------------------------------------

    def _workx_env(self) -> dict[str, str]:
        """把宿主侧的 WORKX_* 配置透传进容器，并固定日志/产物路径。"""
        env = {k: v for k, v in os.environ.items() if k.startswith("WORKX_")}
        # 评测无人值守：关彩色输出，日志固定到可被采集器扫到的路径
        env["WORKX_NO_COLOR"] = "1"
        env["WORKX_LOG_FILE"] = LOG_PATH
        env.setdefault("WORKX_LOG_LEVEL", "info")
        # Issue #121 修复后 headless 才会初始化日志/审计，且**只在显式指定路径时才落盘**
        # （不回落 ~/.workx/logs，避免并发跑题互相覆盖）。所以这两个路径必须显式给，
        # 否则门禁的 #78 标记仍会丢失 —— 采集器的「门禁触发率 / 误报率」就采不到数。
        env.setdefault("WORKX_AUDIT_FILE", AUDIT_PATH)

        # #166 方案 C：按题下发墙钟预算（宿主显式设置则优先，便于单题调试时覆盖）
        if not env.get("WORKX_AGENT_TIMEOUT_SEC"):
            budget = self._resolve_wall_clock_budget()
            if budget:
                env["WORKX_AGENT_TIMEOUT_SEC"] = str(budget)
                self.logger.info(
                    "wall clock budget for this trial: %ss "
                    "(task timeout_sec x %.2f margin; agent converges before the hard timeout)",
                    budget,
                    WALL_CLOCK_MARGIN,
                )

        if not env.get("WORKX_API_KEY"):
            raise RuntimeError(
                "缺少 WORKX_API_KEY：Workx 需要模型凭据才能执行任务。"
                "请在宿主环境设置 WORKX_API_KEY（以及按需设置 WORKX_BASE_URL / WORKX_MODEL），"
                "它们会通过 env 透传进容器。"
            )
        return env

    # ------------------------------------------------------------------
    # #166 方案 C：按题推导墙钟预算
    # ------------------------------------------------------------------

    def _trial_dir(self) -> pathlib.Path | None:
        """定位当前 trial 的产物目录（harbor 在那里落 config.json）。

        `logs_dir` 形如 `<trial_dir>/agent`（多步任务是 `<trial_dir>/<step>/agent`），
        故向上至多 4 层，取第一个含 `config.json` 的目录。
        """
        node = pathlib.Path(self.logs_dir)
        for _ in range(4):
            if (node / "config.json").is_file():
                return node
            if node.parent == node:
                break
            node = node.parent
        return None

    def _resolve_wall_clock_budget(self) -> int | None:
        """推导本题的墙钟预算（秒）= task.timeout_sec x multiplier x WALL_CLOCK_MARGIN。

        链路：trial config.json -> `task.path`（宿主侧题目目录）-> task.toml
        -> `[agent] timeout_sec`。任一步失败都返回 None（= 不下发 = 旧行为），
        **绝不因推导失败中断 trial** —— 拿不到预算只是退回"agent 不知道 deadline"。
        """
        trial_dir = self._trial_dir()
        if trial_dir is None:
            self.logger.debug("trial config.json not found near %s; skip wall clock", self.logs_dir)
            return None

        try:
            trial_cfg = json.loads((trial_dir / "config.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            self.logger.debug("unreadable trial config.json (%s); skip wall clock", exc)
            return None

        task_path = (trial_cfg.get("task") or {}).get("path")
        if not task_path:
            return None

        timeout_sec = _read_agent_timeout_sec(task_path)
        if not timeout_sec:
            self.logger.debug("no [agent] timeout_sec in %s/task.toml; skip wall clock", task_path)
            return None

        # harbor 的 timeout 乘数（`--timeout-multiplier` / `agent_timeout_multiplier`）。
        # trial config.json 用 exclude_defaults 序列化，未设置时字段缺省 → 视为 1.0。
        multiplier = trial_cfg.get("agent_timeout_multiplier") or trial_cfg.get(
            "timeout_multiplier"
        )
        try:
            multiplier = float(multiplier) if multiplier else 1.0
        except (TypeError, ValueError):
            multiplier = 1.0

        budget = int(timeout_sec * multiplier * WALL_CLOCK_MARGIN)
        return budget if budget > 0 else None
