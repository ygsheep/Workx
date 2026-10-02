#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Workx 行为统计采集器 —— Issue #117 验收口径（#78 的行为统计部分）。

采集 docs/agent-harness-assessment.md §3 Step 2b 定义的五项指标：

  1. 验证命令执行率  agent 结束前执行过测试/构建命令的 run 占比（目标 ≥ 90%）
  2. 门禁触发率      其中由「#78 verification not passed」回灌驱动的占比
  3. 降级率          达 verify_max_attempts 后带警告终止的 run 占比
  4. 误报率          探测不到命令（unavailable）直接放行的 run 占比
  5. 成本增幅        门禁开启 vs 关闭的平均 turns / token 增量

产物来源
  stream.jsonl  `workx -p "<task>" --output-format stream-json` 的 stdout（NDJSON）
  *.log         `WORKX_LOG_FILE` 指向的日志文件（门禁 LOG_WARN 标记）

⚠️ 已知限制（2026-10-03 实测确认，追踪 issue #121）
  门禁的 `#78 ...` 标记由 `LOG_WARN` 写出，**只出现在日志文件里**，stream.jsonl 不含。
  而 `src/tui/main.cpp` 的日志初始化块（第 180-221 行）位于 headless 提前 return
  （第 167-170 行）**之后** —— 即 headless 模式根本没初始化文件日志与审计日志，
  `WORKX_LOG_FILE` 被静默忽略，stderr 也是 0 字节。
  → 后果：指标 2（门禁触发率）与指标 4（误报率）在修复前**采不到**。
  本采集器对缺日志的 run 记 `evidence=none`，并在 `evidence_coverage` 里如实报告，
  **绝不把「采不到」当成「没触发」**。

用法
  python collect_metrics.py --runs-dir jobs/2026-10-03 -o metrics.json --md metrics.md
  python collect_metrics.py --runs-dir jobs/on --baseline jobs/off     # 成本增幅
  python collect_metrics.py --self-test                                # 跑内置样例自检
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import Any

# ---------------------------------------------------------------------------
# 常量：与 src/agent/core/goal_verdict.h / src/agent/core/react_loop.cpp 对齐
# ---------------------------------------------------------------------------

GOAL_STATUS = {
    0: "Unknown",
    1: "Pending",
    2: "Achieved",
    3: "Failed",
    4: "NotStarted",
}

MARK_TRIGGER = "#78 verification not passed"
MARK_DEGRADE = "#78 gate degraded"
MARK_SKIPPED = "#78 verification skipped"
MARK_PASSED = "#78 verification passed"

# 命令执行工具名（src/agent/factory.cpp 注册的 BashTool / PowerShellTool）
COMMAND_TOOLS = {"Bash", "PowerShell"}

# 测试/构建工具名 -> 需要匹配的附加条件（None = 出现即算）
_VERIFY_RULES: dict[str, set[str] | None] = {
    "ctest": None,
    "make": None,
    "ninja": None,
    "cmake": {"--build", "-B"},  # cmake -B 是 configure，也计入构建
    "cargo": {"test", "build", "check", "clippy"},
    "go": {"test", "build", "vet"},
    "npm": {"test", "run"},
    "yarn": {"test", "build"},
    "pnpm": {"test", "build"},
    "npx": {"test", "build"},
    "pytest": None,
    "jest": None,
    "vitest": None,
    "mocha": None,
    "rspec": None,
    "rake": None,
    "mvn": None,
    "mvnw": None,
    "gradle": None,
    "gradlew": None,
    "dotnet": {"test", "build"},
    "tsc": None,
    "python": None,
    "python3": None,
}

# python -m pytest / python -m unittest 才算验证
_PY_MODULES = ("pytest", "unittest")

_SEP_RE = re.compile(r"&&|\|\||[;|\n]")


# ---------------------------------------------------------------------------
# 解析辅助
# ---------------------------------------------------------------------------


def iter_jsonl(text: str) -> list[dict[str, Any]]:
    """逐行解析 NDJSON，跳过空行与解析失败行（管道里可能混有非 JSON 输出）。"""
    out: list[dict[str, Any]] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict):
            out.append(obj)
    return out


def extract_command(tool_input: Any) -> str:
    """从 step 的 tool_input 里取出命令原文。

    tool_input 可能是 dict（如 {"command": "..."}），也可能是 JSON 字符串。
    """
    if isinstance(tool_input, dict):
        for key in ("command", "cmd", "script", "input"):
            v = tool_input.get(key)
            if isinstance(v, str):
                return v
        return json.dumps(tool_input, ensure_ascii=False)
    s = tool_input if isinstance(tool_input, str) else str(tool_input)
    stripped = s.strip()
    if stripped.startswith("{"):
        try:
            obj = json.loads(stripped)
        except json.JSONDecodeError:
            return s
        if isinstance(obj, dict):
            for key in ("command", "cmd", "script", "input"):
                v = obj.get(key)
                if isinstance(v, str):
                    return v
    return s


def split_commands(cmd: str) -> list[str]:
    """按 shell 运算符切分，返回各段命令（去首尾空白）。"""
    return [seg.strip() for seg in _SEP_RE.split(cmd) if seg.strip()]


def command_is_verification(cmd: str) -> bool:
    """判断一条 Bash/PowerShell 命令是否为测试或构建命令。

    判定口径：先按 && || ; | 切段，再看每段的**首个词**是否命中已知工具，
    避免把 "make a sandwich" 这类自然语言误判成 make。
    """
    for seg in split_commands(cmd):
        # 去掉前置的环境变量赋值（如 FOO=1 ctest）
        tokens = [t for t in seg.split() if t]
        while tokens and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", tokens[0]):
            tokens.pop(0)
        if not tokens:
            continue
        base = Path(tokens[0]).name  # ./gradlew -> gradlew
        if base.endswith(".cmd") or base.endswith(".exe"):
            base = base.rsplit(".", 1)[0]
        rule = _VERIFY_RULES.get(base)
        if rule is None:
            if base in _VERIFY_RULES:
                return True
            if base in ("python", "python3"):
                rest = " ".join(tokens[1:])
                if re.search(r"-m\s+(" + "|".join(_PY_MODULES) + r")\b", rest):
                    return True
            continue
        rest = " ".join(tokens[1:])
        if any(tok in rest for tok in rule):
            return True
        # cmake -B 可能是 "-B build"，rule 里有 "-B" 直接命中；上面已覆盖
    return False


# ---------------------------------------------------------------------------
# 单次 run 的解析
# ---------------------------------------------------------------------------


def parse_run(stream_path: Path, log_path: Path | None) -> dict[str, Any]:
    steps = iter_jsonl(stream_path.read_text(encoding="utf-8", errors="replace"))

    commands: list[str] = []
    goal_status: int | None = None
    usage: dict[str, Any] = {}

    for obj in steps:
        if "type" in obj:
            if obj.get("type") == "action":
                if obj.get("tool_name") in COMMAND_TOOLS:
                    commands.append(extract_command(obj.get("tool_input")))
            continue
        # 末行结果对象：含 usage / goal_status
        if "usage" in obj:
            usage = obj.get("usage") or {}
            gs = obj.get("goal_status")
            if isinstance(gs, int):
                goal_status = gs

    log_text = log_path.read_text(encoding="utf-8", errors="replace") if log_path else ""

    return {
        "run": stream_path.stem,
        "stream": str(stream_path),
        "log": str(log_path) if log_path else None,
        "commands": commands,
        "verification_command": any(command_is_verification(c) for c in commands),
        "gate_triggered": MARK_TRIGGER in log_text,
        "gate_degraded": MARK_DEGRADE in log_text or goal_status == 3,
        "gate_skipped": MARK_SKIPPED in log_text,
        "gate_passed": MARK_PASSED in log_text,
        "goal_status": goal_status,
        "goal_status_name": GOAL_STATUS.get(goal_status, "?") if goal_status is not None else None,
        "total_iterations": usage.get("total_iterations"),
        "total_tool_calls": usage.get("total_tool_calls"),
        "prompt_tokens": usage.get("prompt_tokens"),
        "generated_tokens": usage.get("generated_tokens"),
        "duration_ms": usage.get("duration_ms"),
        "has_log_evidence": log_path is not None,
    }


def collect(stream_files: list[Path]) -> dict[str, Any]:
    runs: list[dict[str, Any]] = []
    for sp in stream_files:
        log = find_log_for(sp)
        runs.append(parse_run(sp, log))

    total = len(runs)
    with_log = sum(1 for r in runs if r["has_log_evidence"])

    def rate(key: str, require_log: bool = False) -> dict[str, Any]:
        pool = [r for r in runs if (not require_log or r["has_log_evidence"])]
        n = len(pool)
        hit = sum(1 for r in pool if r[key])
        return {
            "numerator": hit,
            "denominator": n,
            "value": (hit / n) if n else None,
            "evidence": "log" if require_log else "stream",
            "uncovered": (total - n) if require_log else 0,
        }

    m1 = rate("verification_command")
    m2 = rate("gate_triggered", require_log=True)
    m3 = rate("gate_degraded")
    m4 = rate("gate_skipped", require_log=True)

    m1["target"] = 0.90
    m1["met"] = (m1["value"] is not None and m1["value"] >= 0.90)

    return {
        "schema_version": 1,
        "runs_total": total,
        "evidence_coverage": {
            "stream": total,
            "log": with_log,
            "log_missing": total - with_log,
            "note": "门禁触发率/误报率依赖日志；log_missing>0 时这两项分母被缩小，不可直接当作全量结论",
        },
        "metrics": {
            "verification_command_rate": m1,
            "gate_trigger_rate": m2,
            "degrade_rate": m3,
            "false_positive_rate": m4,
        },
        "cost": aggregate_cost(runs),
        "per_run": runs,
    }


def find_log_for(stream_path: Path) -> Path | None:
    """找与 stream 同目录、同 stem 的 .log；其次同目录任意 .log。"""
    direct = stream_path.with_suffix(".log")
    if direct.exists():
        return direct
    cands = sorted(stream_path.parent.glob("*.log"))
    return cands[0] if len(cands) == 1 else None


def aggregate_cost(runs: list[dict[str, Any]]) -> dict[str, Any]:
    fields = ("total_iterations", "total_tool_calls", "prompt_tokens", "generated_tokens", "duration_ms")
    out: dict[str, Any] = {}
    for f in fields:
        vals = [r[f] for r in runs if isinstance(r.get(f), (int, float))]
        out[f] = {
            "mean": (sum(vals) / len(vals)) if vals else None,
            "n": len(vals),
        }
    return out


def compare_cost(a: dict[str, Any], b: dict[str, Any]) -> dict[str, Any]:
    """a = 门禁开启，b = 基线（门禁关闭）。返回增幅。"""
    res: dict[str, Any] = {}
    for f, sa in a.get("cost", {}).items():
        sb = b.get("cost", {}).get(f, {})
        ma, mb = sa.get("mean"), sb.get("mean")
        if ma is None or mb is None:
            res[f] = {"gate_on": ma, "baseline": mb, "delta": None, "pct": None}
            continue
        delta = ma - mb
        res[f] = {
            "gate_on": ma,
            "baseline": mb,
            "delta": delta,
            "pct": (delta / mb * 100.0) if mb else None,
        }
    return res


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------


def to_markdown(res: dict[str, Any], baseline: dict[str, Any] | None = None) -> str:
    m = res["metrics"]
    cov = res["evidence_coverage"]
    lines: list[str] = []
    lines.append("# Workx 行为统计采集结果（Issue #117）\n")
    lines.append(f"- run 总数：**{res['runs_total']}**")
    lines.append(f"- 日志证据：{cov['log']} / {cov['stream']}（缺 {cov['log_missing']}）\n")

    lines.append("| 指标 | 值 | 计数 | 分母 | 证据源 |")
    lines.append("| --- | --- | --- | --- | --- |")

    def pct(v: float | None) -> str:
        return "—" if v is None else f"{v * 100:.1f}%"

    labels = {
        "verification_command_rate": "验证命令执行率",
        "gate_trigger_rate": "门禁触发率",
        "degrade_rate": "降级率",
        "false_positive_rate": "误报率（unavailable 放行）",
    }
    for key, label in labels.items():
        d = m[key]
        lines.append(
            f"| {label} | {pct(d['value'])} | {d['numerator']} | {d['denominator']} | {d['evidence']} |"
        )

    v = m["verification_command_rate"]
    verdict = (
        "✅ 达标"
        if v["met"]
        else ("⚠️ 未达标（< 90%，按 #117 另开针对性 issue）" if v["value"] is not None else "—")
    )
    lines.append(f"\n**验证命令执行率判定（目标 ≥ 90%）：{verdict}**\n")

    if cov["log_missing"]:
        lines.append(
            f"> ⚠️ 有 {cov['log_missing']} 个 run 缺日志证据，"
            "「门禁触发率」与「误报率」的分母已相应缩小，**不可外推到全量**。\n"
        )

    c = res["cost"]
    lines.append("| 成本项 | 均值 |")
    lines.append("| --- | --- |")
    for f, d in c.items():
        mean = d["mean"]
        lines.append(f"| {f} | {'—' if mean is None else f'{mean:,.1f}'} |")

    if baseline is not None:
        lines.append("\n| 成本项 | 门禁开启 | 基线 | 增幅 | 增幅% |")
        lines.append("| --- | --- | --- | --- | --- |")
        for f, d in compare_cost(res, baseline).items():
            lines.append(
                f"| {f} | {d['gate_on']} | {d['baseline']} | {d['delta']} | "
                f"{'—' if d['pct'] is None else f'{d["pct"]:.1f}%' } |"
            )

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def expand_paths(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            out.extend(sorted(path.rglob("*.jsonl")))
        else:
            out.append(path)
    return out


def self_test() -> int:
    """用内置样例自检，确认解析链路可用（不是跑分，是协议验证）。"""
    fx = Path(__file__).parent / "fixtures"
    files = sorted(fx.glob("*.jsonl"))
    if not files:
        print("FAIL: 缺少 fixtures", file=sys.stderr)
        return 1
    res = collect(files)
    m = res["metrics"]
    expect = {
        "verification_command_rate": (2, 3),
        "gate_trigger_rate": (2, 3),
        "degrade_rate": (1, 3),
        "false_positive_rate": (0, 3),
    }
    ok = True
    for key, (num, den) in expect.items():
        got = (m[key]["numerator"], m[key]["denominator"])
        status = "PASS" if got == (num, den) else "FAIL"
        if got != (num, den):
            ok = False
        print(f"[{status}] {key}: 期望 {num}/{den}，实得 {got[0]}/{got[1]}")
    print(f"[{'PASS' if ok else 'FAIL'}] self-test 总体")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Workx 行为统计采集器（Issue #117）")
    ap.add_argument("paths", nargs="*", help="stream.jsonl 文件或包含它们的目录")
    ap.add_argument("--runs-dir", help="run 产物根目录（递归找 *.jsonl）")
    ap.add_argument("--baseline", help="对照组目录（门禁关闭），用于成本增幅")
    ap.add_argument("-o", "--out", help="输出 JSON 路径")
    ap.add_argument("--md", help="输出 Markdown 路径")
    ap.add_argument("--self-test", action="store_true", help="用内置样例自检")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    paths = list(args.paths)
    if args.runs_dir:
        paths.append(args.runs_dir)
    if not paths:
        ap.error("需要 paths / --runs-dir，或用 --self-test 自检")

    files = expand_paths(paths)
    if not files:
        print("未找到任何 *.jsonl", file=sys.stderr)
        return 1

    res = collect(files)
    baseline = collect(expand_paths([args.baseline])) if args.baseline else None
    if baseline is not None:
        res["cost_delta_vs_baseline"] = compare_cost(res, baseline)

    text = json.dumps(res, ensure_ascii=False, indent=2)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"已写入 {args.out}")
    md = to_markdown(res, baseline)
    if args.md:
        Path(args.md).write_text(md, encoding="utf-8")
        print(f"已写入 {args.md}")
    if not args.out and not args.md:
        print(md)

    m = res["metrics"]["verification_command_rate"]
    return 0 if (m["value"] is None or m["met"]) else 3  # 3 = 未达标，供 CI 判定


if __name__ == "__main__":
    raise SystemExit(main())
