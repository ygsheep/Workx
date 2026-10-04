#!/usr/bin/env python
"""校验待评测二进制真的含目标改动，别拿着旧产物跑对照实验。

由来（2026-10-04，教训很贵）：
    曾用一份 10-03 13:18 的二进制去验证 10-03 20:47 才合并的 #133 修复，
    于是得出「#133 有效」的假结论 —— 期间所有差异其实只是 LLM 采样随机性。
    对照实验的第一道门必须是**产物签名**，而不是「我记得编过了」。

用法：
    python scripts/harness/check_binary_signature.py build/linux/workx-linux-amd64
    python scripts/harness/check_binary_signature.py <bin> --only-version   # 只看 git 版本串
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

#: 特征字符串 -> 它代表哪个改动。**改ID时记得同步这里。**
#: ⚠️ 新增特征前**必须先在真实二进制里 grep 到**：「看着该有」不等于真的在，
#: 比如 WORKX_TIMEOUT 明明写死在 app_config.cpp 的 env_var 里，却在产物中搜不到
#: （字符串表优化），把它列为必选项就会误报——会让真正有用的校验失去可信度。
MARKS: dict[str, str] = {
    "WORKX_AGENT_TIMEOUT_SEC": "#137 墙钟预算环境变量",
    "remaining wall clock": "#137 单条 shell 按剩余墙钟封顶告警",
    "Strict": "#85 权限模式的严格档",
    "PermissionMode": "权限模式基线（旧版本也有，用来确认没裁剪错）",
}

#: 二进制里 impronta 的 git hash，形如 `-g9c4cba39`
GIT_RE = re.compile(r"-g([0-9a-f]{7,40})")


def main() -> int:
    ap = argparse.ArgumentParser(description="校验待评测二进制的改动签名")
    ap.add_argument("binary", type=Path, help="待评测的 workx 二进制路径")
    ap.add_argument(
        "--only-version", action="store_true", help="只打印源码版本串，不做特征检查"
    )
    args = ap.parse_args()

    path: Path = args.binary
    if not path.is_file():
        print(f"❌ 二进制不存在：{path}")
        return 1

    blob = path.read_bytes()
    print(f"二进制 {path}")
    print(f"  size = {len(blob):,} bytes")
    print(f"  mtime= {os.path.getmtime(path):.0f}")

    m = GIT_RE.search(blob.decode("latin-1", errors="replace"))
    if m:
        print(f"  源码版本 = -g{m.group(1)}（用 `git log -1 <hash>` 确认来源）")
    else:
        print("  源码版本 = <未找到 -g<hash> 标记>")

    if args.only_version:
        return 0

    missing = [name for name in MARKS if name.encode() not in blob]
    print("\n特征签名：")
    for name, desc in MARKS.items():
        ok = name.encode() in blob
        print(f"  {'✅' if ok else '❌'} {name:28} {desc}")

    if missing:
        print(
            f"\n❌ 缺少 {len(missing)} 个标记 —— 说明改动没编进去，"
            "拿它跑出来的对照结论不可信。"
        )
        return 1

    print("\n✅ 二进制含全部目标改动标记")
    return 0


if __name__ == "__main__":
    sys.exit(main())
