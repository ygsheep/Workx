#!/usr/bin/env python
"""Terminal-Bench 数据集行尾守卫（CRLF → LF）。

起因
    Issue #142 复跑时发现 **Windows 上的 `core.autocrlf=true`** 会把 harbor 拉下来的
    Terminal-Bench 数据集在 checkout 时整体转成 CRLF，于是：

      * 依赖 md5/sha 校验的题（`test_protected_files_not_modified` 一类）
        **必然失败** —— 期望值算的是 LF 版本；
      * 容器里 `#!/bin/bash^M` 会因 `bad interpreter` 直接跑不起来，
        表现为「verifier 没跑完」这类**看起来像 agent 失败**的噪声。

    实测 `.cache/tb2/terminal-bench` 下 **736 / 759** 个文件被污染，
    即此前每一轮跑分都带这个系统性偏差。

用法
    python scripts/harness/fix_dataset_lineendings.py --check    # 只报告，非 0 退出
    python scripts/harness/fix_dataset_lineendings.py --fix      # 就地转换
    python scripts/harness/fix_dataset_lineendings.py --check --data-dir <path>

判别规则
    含 NUL 字节的文件视为二进制，**绝不改写**（图片 / 压缩包 / so 里出现 \\r\\n 是正常的）。
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

DEFAULT_DATA_DIR = Path(".cache/tb2/terminal-bench")


def scan(root: Path) -> tuple[list[Path], int, int]:
    """返回 (需修复的文本文件, 跳过的二进制数, 读取失败数)。"""
    need_fix: list[Path] = []
    binary = failed = 0
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in filenames:
            path = Path(dirpath) / name
            try:
                blob = path.read_bytes()
            except OSError:
                failed += 1
                continue
            if b"\x00" in blob:
                binary += 1
                continue
            if b"\r\n" in blob:
                need_fix.append(path)
    return need_fix, binary, failed


def main() -> int:
    ap = argparse.ArgumentParser(description="Terminal-Bench 数据集行尾守卫")
    ap.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR)
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--check", action="store_true", help="只检查不修改（默认）")
    group.add_argument("--fix", action="store_true", help="就地转换 CRLF -> LF")
    args = ap.parse_args()

    root: Path = args.data_dir
    if not root.is_dir():
        print(f"⚠️ 数据集目录不存在：{root} —— 跳过行尾检查（harbor 可能会自行拉取）")
        return 0

    need_fix, binary, failed = scan(root)
    print(f"扫描 {root}：待修文本 {len(need_fix)} / 二进制跳过 {binary} / 读取失败 {failed}")

    if not need_fix:
        print("✅ 数据集行尾正常（LF）")
        return 0

    if args.fix:
        for path in need_fix:
            path.write_bytes(path.read_bytes().replace(b"\r\n", b"\n"))
        print(f"✅ 已把 {len(need_fix)} 个文件的 CRLF 转成 LF")
        return 0

    print("\n".join(f"   {p}" for p in need_fix[:10]))
    if len(need_fix) > 10:
        print(f"   ...（另有 {len(need_fix) - 10} 个）")
    print(
        "❌ 数据集是 CRLF：checksum 校验类题目必然失败，容器里 *.sh 也可能"
        " `bad interpreter`。请先 `python scripts/harness/fix_dataset_lineendings.py --fix`"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
