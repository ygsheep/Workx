#!/usr/bin/env python3
"""验证 workx-prep/* 预装镜像的依赖真的可用（#176 闭环校验）。

背景
    scripts/harness/env-prep.json + prepare_envs.py 会给 Terminal-Bench 的裸镜像题
    追加预装层，但「Dockerfile 里写了 RUN」≠「容器里能跑」——apt 可能被代理/网络
    静默跳过、包可能装到了非默认 PATH（stockfish 在 /usr/games）。本脚本逐题
    `docker run` 探测，把「预装是否真的生效」变成可重复的硬证据。

用法
    MSYS_NO_PATHCONV=1 python scripts/harness/verify_prep_images.py
    MSYS_NO_PATHCONV=1 python scripts/harness/verify_prep_images.py --json out.json

注意
    ⚠️ 必须 MSYS_NO_PATHCONV=1：否则 Git Bash 会把 `--entrypoint /bin/bash` 里的
    /bin/bash 转成 Windows 路径，容器起不来（表现为 "no such file or directory"）。
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path

# 每题一条探测命令：命中 0 即通过。命令尽量选「非登录 shell 也能跑」的形式，
# 因为 agent 在容器里同样不是登录 shell —— 这能暴露 PATH 类问题（如 /usr/games）。
CHECKS: dict[str, tuple[str, str]] = {
    "adaptive-rejection-sampler": ("R + openssl", "command -v Rscript && openssl version"),
    "caffe-cifar-10": ("protoc + boost + cmake", "command -v protoc && ls /usr/include/boost >/dev/null && command -v cmake"),
    "configure-git-webserver": ("git + nginx + sshd", "command -v git && command -v nginx && command -v sshd"),
    "extract-moves-from-video": ("ffmpeg + yt-dlp + tesseract", "command -v ffmpeg && command -v yt-dlp && command -v tesseract"),
    "chess-best-move": ("stockfish + python-chess + numpy", "command -v stockfish && python3 -c 'import chess,numpy'"),
    "crack-7z-hash": ("7z + perl LZMA", "command -v 7z && perl -MCompress::Raw::Lzma -e 1"),
    "cobol-modernization": ("gnucobol (cobc)", "command -v cobc"),
    "count-dataset-tokens": ("datasets + transformers + jinja2", "python3 -c 'import datasets,transformers,jinja2'"),
    "financial-document-processor": ("tesseract-ocr", "command -v tesseract"),
}

IMAGE_TMPL = "workx-prep/{task}:latest"


def check_image(task: str, desc: str, cmd: str) -> tuple[bool, str]:
    image = IMAGE_TMPL.format(task=task)
    proc = subprocess.run(
        ["docker", "run", "--rm", "--entrypoint", "/bin/bash", image, "-c", cmd],
        capture_output=True,
        text=True,
        timeout=300,
    )
    if proc.returncode == 0:
        return True, desc
    detail = (proc.stderr or proc.stdout).strip().splitlines()
    return False, detail[0] if detail else "非零退出"


def main() -> int:
    parser = argparse.ArgumentParser(description="校验预装镜像依赖可用性")
    parser.add_argument("--json", help="把结果另存为 JSON")
    args = parser.parse_args()

    if shutil.which("docker") is None:
        print("❌ 找不到 docker", file=sys.stderr)
        return 2

    results: dict[str, dict[str, object]] = {}
    failed = 0
    for task, (desc, cmd) in CHECKS.items():
        ok, detail = check_image(task, desc, cmd)
        results[task] = {"ok": ok, "desc": desc, "detail": detail}
        mark = "✅" if ok else "❌"
        print(f"{IMAGE_TMPL.format(task=task):<46} {mark} {desc if ok else detail}")
        if not ok:
            failed += 1

    print(f"\n{'✅ 全部通过' if not failed else f'❌ {failed} 题未通过'}（共 {len(CHECKS)} 题）")
    if args.json:
        Path(args.json).write_text(
            json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8", newline="\n"
        )
        print(f"结果已写入 {args.json}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
