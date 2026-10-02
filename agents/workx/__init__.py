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

⚠️ 未验证声明
    本适配器在**没有可用 Docker 守护进程**的机器上编写（WSL 被安全策略拉黑），
    `install()` / `run()` 的实际容器内行为**尚未实跑验证**。首次使用请先跑单题 smoke：
        harbor run -d terminal-bench@2.0 -a agents.workx:WorkxAgent \\
                   --include-task-name git-init -k 1
"""

from __future__ import annotations

import base64
import os
import shlex
from typing import Any

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

#: 运行时系统依赖（Terminal-Bench 任务镜像多为 Ubuntu）
APT_PACKAGES = "ca-certificates curl git libcurl4-openssl-dev tzdata"

#: 源码构建所需的编译依赖
BUILD_PACKAGES = (
    "build-essential cmake ninja-build pkg-config unzip zip tar "
    "libcurl4-openssl-dev python3"
)


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
        # 预构建二进制 URL：给了就走快路径，不给就容器内源码构建
        self.prebuilt_url = os.environ.get("WORKX_AGENT_BINARY_URL", "")
        self.max_iterations = os.environ.get("WORKX_AGENT_MAX_ITERATIONS", "")
        self.cwd = os.environ.get("WORKX_AGENT_CWD", "")
        self.permission_mode = os.environ.get(
            "WORKX_AGENT_PERMISSION_MODE", "bypass-permissions"
        )

    # ------------------------------------------------------------------
    # 元数据
    # ------------------------------------------------------------------

    @staticmethod
    def name() -> str:
        return "workx"

    def version(self) -> str:
        return "0.10.1"

    # ------------------------------------------------------------------
    # 安装
    # ------------------------------------------------------------------

    async def install(self, environment: Any) -> None:
        """容器内的两阶段安装：系统依赖 → workx 本体。"""
        await self.exec_as_root(
            environment,
            command=f"apt-get update -qq && apt-get install -y --no-install-recommends "
            f"{APT_PACKAGES} {BUILD_PACKAGES}",
        )

        if self.prebuilt_url:
            await self.exec_as_root(
                environment,
                command=f"curl -fsSL {shlex.quote(self.prebuilt_url)} -o {BIN_PATH} "
                f"&& chmod 0755 {BIN_PATH} && {BIN_PATH} --version",
            )
        else:
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

        if not env.get("WORKX_API_KEY"):
            raise RuntimeError(
                "缺少 WORKX_API_KEY：Workx 需要模型凭据才能执行任务。"
                "请在宿主环境设置 WORKX_API_KEY（以及按需设置 WORKX_BASE_URL / WORKX_MODEL），"
                "它们会通过 env 透传进容器。"
            )
        return env
