#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Workx 行为统计采集器 —— Issue #117 验收口径（#78 的行为统计部分）。

采集 docs/agent-harness-assessment.md §3 Step 2b 定义的五项指标：

  1. 验证命令执行率  agent 结束前执行过测试/构建命令的 run 占比（目标 ≥ 90%）
     #132 起三档分别报数（层层包含，不合并成一个数）：
       strict  可识别的测试/构建运行器（pytest / ctest / make test / npm test …）
       loose   + 自写测试脚本（python3 test_xxx.py / bash test_xxx.sh）
       probe   + 一次性探针（python3 -c、od -c、sqlite3 查库、diff 比对、
               临时 probe*.sh、直接运行 ./ 产物）
  2. 门禁触发率      其中由「#78 verification not passed」回灌驱动的占比
  3. 降级率          达 verify_max_attempts 后带警告终止的 run 占比
  4. 误报率          探测不到命令（unavailable）直接放行的 run 占比
  5. 成本增幅        门禁开启 vs 关闭的平均 turns / token 增量

#132 场景判定（--scenario，决定达标判定用哪一档）：
  repo       真实工程仓库 → strict 档，≥90%（#117 原口径）
  benchmark  Terminal-Bench 类空目录做题 → probe 档「结束前有过至少一次
             针对性自检动作」——verifier 测试跑在独立容器，agent 拿不到
             tests/，跑可识别运行器先天不适用；门禁触发率在此场景结构性
             恒零（无工程标记 → 无 goal），不参与判定。
             ⚠️ 但不要放宽识别表到「任何命令都算验证」——那等于取消口径。

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
from typing import Any, Iterable

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

# ---------------------------------------------------------------------------
# 场景判定口径（#132 建议动作 a）：
#   repo       真实工程仓库（有 CMakeLists / package.json / Makefile）
#              → 判定档 = strict（可识别运行器），目标 ≥90%（#117 原口径）
#   benchmark  Terminal-Bench 类空目录做题：verifier 测试跑在独立容器，
#              agent 拿不到 tests/，跑可识别运行器**先天不适用**
#              → 判定档 = probe（结束前有过至少一次针对性自检动作）
# 门禁触发率（gate_trigger_rate）在这套题上是**结构性恒零**（容器里无工程标记，
# detect_default_goal() 返回 None），不用于判达标/不达标。
# ---------------------------------------------------------------------------
SCENARIOS: dict[str, dict[str, Any]] = {
    "repo": {"name": "repo", "decision_tier": "strict", "target": 0.90},
    "benchmark": {"name": "benchmark", "decision_tier": "probe", "target": 0.90},
}

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
    # ⚠️ python / python3 **不在这里**。2026-10-03 首轮真实跑分发现：
    # 把它们当「出现即算」会让 `python3 --version` 也被判成验证命令，
    # 验证命令执行率被抬到 100%（假阳性）。python 族走下面的 _PY_* 规则。
}

# python / python3 只在**真的在跑测试**时才算验证
_PY_BASENAMES = {"python", "python3", "python2", "py"}
_PY_MODULES = ("pytest", "unittest")
_PY_RUNNER_RE = re.compile(r"(?:^|\s)(pytest|unittest)\b")
#: 自写测试脚本（宽松口径）：python3 test_regex.py 这类确实是在验证，
#: 但不是「识别得出的运行器」，故只计入宽松口径，不计入达标判定口径。
_PY_TESTSCRIPT_RE = re.compile(r"(?:^|[\s/])(test_[^/\s]*\.py|[^/\s]*_test\.py|conftest\.py)\b")

# ---------------------------------------------------------------------------
# #132 新增：probe 档（一次性探针）。
# 2026-10-03 首轮 20 题实测：agent 在 Terminal-Bench 的空目录场景里**确实在自检**，
# 但用的是一次性探针（python3 -c 查 sqlite、od -c 看文件、临时 probe*.sh 差分），
# 严格/宽松两档都命中不了 → 两档同分 6.7%，宽松档形同虚设。
# probe 档把「直接运行产物并比对输出」这类针对性自检动作也算进来，
# **三档分别报数、不合并成一个数**，供不同场景选判定档（见 SCENARIOS）。
# ---------------------------------------------------------------------------

#: python 族的一次性内联执行：`python3 -c '...'` / `python3 -`（stdin 喂脚本）
_PY_INLINE_FLAGS = {"-c", "-"}

#: 直接检查/查询产物的工具：od/xxd/hexdump 看字节，sqlite3 直接查库
_PROBE_INSPECT_TOOLS = {"od", "xxd", "hexdump", "sqlite3"}

#: 输出比对：diff/cmp/comm 是「运行产物 → 比对输出」探针的收尾动作
_PROBE_DIFF_TOOLS = {"diff", "cmp", "comm"}

#: 临时探针脚本（首轮实跑真实命名：probe.sh / probe_diff.sh / probe1.sh …）
_PROBE_SCRIPT_RE = re.compile(r"(?:^|[\s/])probe[^/\s]*\.sh\b")

#: 自写测试脚本的 shell 版（宽松口径扩展到 bash/sh：bash test_x.sh）
_SH_TESTSCRIPT_RE = re.compile(r"(?:^|[\s/])(test_[^/\s]*\.sh|[^/\s]*_test\.sh)\b")

#: 直接运行当前目录产物（./a.out、./app --check）：跑分容器里最典型的探针
_LOCAL_EXEC_RE = re.compile(r"^\./\S")

# 只是问版本/用法，不是在执行测试或构建
_NON_RUN_FLAGS = {"--version", "-V", "--help", "-h", "--usage"}

_SEP_RE = re.compile(r"&&|\|\||[;|\n]")

#: 命令验证档位（数值越大越宽松；strict ⊂ loose ⊂ probe）
_LEVEL_NONE, _LEVEL_STRICT, _LEVEL_LOOSE, _LEVEL_PROBE = 0, 1, 2, 3


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


def command_verification_tiers(cmd: str) -> tuple[bool, bool, bool]:
    """判断一条 Bash/PowerShell 命令命中了哪些验证档**类**，返回 (strict, loose, probe)。

    判定口径：先按 && || ; | 切段，再看每段的**首个词**是否命中已知工具，
    避免把 "make a sandwich" 这类自然语言误判成 make。

    返回的是**档类原始命中**（互斥的三类模式，非累积）：
      strict  可识别的测试/构建运行器（pytest / ctest / make test / npm test …）
      loose   自写测试脚本（python3 test_xxx.py / bash test_xxx.sh）
      probe   一次性探针：python3 -c / od -c / sqlite3 查库 / diff 比对 /
              临时 probe*.sh / 直接运行 ./ 产物
    累积口径（probe ⊇ loose ⊇ strict）由调用方合并，见 command_verification_tiers_cumulative。
    """
    hit_strict = hit_loose = hit_probe = False
    for seg in split_commands(cmd):
        # 去掉前置的环境变量赋值（如 FOO=1 ctest）
        tokens = [t for t in seg.split() if t]
        while tokens and re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", tokens[0]):
            tokens.pop(0)
        if not tokens:
            continue
        rest_tokens = tokens[1:]
        # python3 --version / cmake --help：只是问询，不是执行
        if _NON_RUN_FLAGS.intersection(rest_tokens):
            continue
        base = Path(tokens[0]).name  # ./gradlew -> gradlew
        if base.endswith(".cmd") or base.endswith(".exe"):
            base = base.rsplit(".", 1)[0]
        rest = " ".join(rest_tokens)

        if base in _PY_BASENAMES:
            if not rest:
                continue
            if re.search(r"-m\s+(" + "|".join(_PY_MODULES) + r")\b", rest):
                hit_strict = True
            elif _PY_RUNNER_RE.search(rest):
                hit_strict = True
            elif _PY_TESTSCRIPT_RE.search(rest):
                hit_loose = True
            elif _PY_INLINE_FLAGS.intersection(rest_tokens):
                # python3 -c '...' / python3 -：一次性内联探针
                hit_probe = True
        elif base in _VERIFY_RULES:
            rule = _VERIFY_RULES[base]
            if rule is None or any(tok in rest for tok in rule):
                hit_strict = True
        elif base in _PROBE_INSPECT_TOOLS:
            # od / xxd / hexdump / sqlite3：直接检查产物内容（问询已被上面排除）
            hit_probe = True
        elif base in _PROBE_DIFF_TOOLS:
            # diff / cmp：比对产物输出
            hit_probe = True
        elif base in {"bash", "sh", "zsh", "dash"}:
            if _PROBE_SCRIPT_RE.search(rest):
                hit_probe = True
            elif _SH_TESTSCRIPT_RE.search(rest):
                hit_loose = True
        elif _PROBE_SCRIPT_RE.search(" " + tokens[0] + " "):
            # probe.sh 直接执行（首词本身是探针脚本名）
            hit_probe = True

        # 直接运行当前目录产物：./a.out、./app --check（首轮实跑最典型探针）
        if any(_LOCAL_EXEC_RE.match(t) for t in tokens):
            hit_probe = True

        if hit_strict:
            break  # 命中最高档类，无需继续切分
    return hit_strict, hit_loose, hit_probe


def tiers_cumulative(
    tiers: Iterable[tuple[bool, bool, bool]],
) -> tuple[bool, bool, bool]:
    """把多档类原始命中合并成累积口径（strict ⊂ loose ⊂ probe）。"""
    hit_strict = any(t[0] for t in tiers)
    hit_loose = any(t[0] or t[1] for t in tiers)
    hit_probe = any(t[0] or t[1] or t[2] for t in tiers)
    return hit_strict, hit_loose, hit_probe


def command_verification_level(cmd: str) -> int:
    """单档视图（自检/调试用）：命中的最高档类。"""
    strict, loose, probe = command_verification_tiers(cmd)
    if strict:
        return _LEVEL_STRICT
    if loose:
        return _LEVEL_LOOSE
    if probe:
        return _LEVEL_PROBE
    return _LEVEL_NONE


def command_is_verification(cmd: str, loose: bool = False) -> bool:
    """兼容入口：命令是否达到验证档位。

    @param loose False = 严格档（可识别运行器）；True = 宽松档（含自写测试脚本）。
    """
    strict, loose_tier, _ = tiers_cumulative([command_verification_tiers(cmd)])
    return loose_tier if loose else strict


# ---------------------------------------------------------------------------
# 单次 run 的解析
# ---------------------------------------------------------------------------


def parse_run(stream_path: Path, log_path: Path | None) -> dict[str, Any]:
    steps = iter_jsonl(stream_path.read_text(encoding="utf-8", errors="replace"))

    commands: list[str] = []
    goal_status: int | None = None
    usage: dict[str, Any] = {}
    step_count = 0

    for obj in steps:
        if "type" in obj:
            step_count += 1
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

    tiers = [command_verification_tiers(c) for c in commands]
    strict_hit, loose_hit, probe_hit = tiers_cumulative(tiers)
    return {
        "run": stream_path.stem,
        "stream": str(stream_path),
        "log": str(log_path) if log_path else None,
        "commands": commands,
        # 三档分别报数（#132），层层包含：strict ⊂ loose ⊂ probe
        "verification_command": strict_hit,
        "verification_command_loose": loose_hit,
        "verification_command_probe": probe_hit,
        "gate_triggered": MARK_TRIGGER in log_text,
        "gate_degraded": MARK_DEGRADE in log_text or goal_status == 3,
        "gate_skipped": MARK_SKIPPED in log_text,
        "gate_passed": MARK_PASSED in log_text,
        "goal_status": goal_status,
        "goal_status_name": GOAL_STATUS.get(goal_status, "?") if goal_status is not None else None,
        # ⚠️ agent 被超时杀掉时 tee 出来的 stream 会是 **0 字节**（容器没了，产物没落盘）。
        #   这种 run 不是「没跑验证命令」，是**根本不知道它跑了什么** ——
        #   按本文件的诚实性原则，必须从验证类指标的分母里剔除，不能当成阴性样本。
        "step_count": step_count,
        "has_stream_evidence": step_count > 0,
        "total_iterations": usage.get("total_iterations"),
        "total_tool_calls": usage.get("total_tool_calls"),
        "prompt_tokens": usage.get("prompt_tokens"),
        "generated_tokens": usage.get("generated_tokens"),
        "duration_ms": usage.get("duration_ms"),
        "has_log_evidence": log_path is not None,
    }


def collect(stream_files: list[Path], scenario_name: str = "repo") -> dict[str, Any]:
    if scenario_name not in SCENARIOS:
        raise ValueError(f"未知场景 '{scenario_name}'，可选：{sorted(SCENARIOS)}")
    runs: list[dict[str, Any]] = []
    for sp in stream_files:
        log = find_log_for(sp)
        runs.append(parse_run(sp, log))

    total = len(runs)
    with_log = sum(1 for r in runs if r["has_log_evidence"])

    def rate(key: str, require_log: bool = False,
             require_steps: bool = False) -> dict[str, Any]:
        pool = [
            r for r in runs
            if (not require_log or r["has_log_evidence"])
            and (not require_steps or r["has_stream_evidence"])
        ]
        n = len(pool)
        hit = sum(1 for r in pool if r[key])
        excluded = total - n
        return {
            "numerator": hit,
            "denominator": n,
            "value": (hit / n) if n else None,
            "evidence": "log" if require_log else "stream",
            "uncovered": excluded if (require_log or require_steps) else 0,
            "uncovered_reason": (
                "log_missing" if require_log
                else ("stream_empty" if require_steps else "")
            ),
        }

    # 验证类指标要求「轨迹里确实有步」——0 字节的 stream 不算阴性样本
    m1 = rate("verification_command", require_steps=True)
    m2 = rate("gate_triggered", require_log=True)
    m3 = rate("gate_degraded")
    m4 = rate("gate_skipped", require_log=True)

    # 宽松口径（含自写测试脚本）与 probe 口径（含一次性探针）只作参考，
    # 三档分别报数、不合并成一个数（#132）。
    m1_loose = rate("verification_command_loose", require_steps=True)
    m1_probe = rate("verification_command_probe", require_steps=True)

    # 场景判定（#132 建议动作 a）：判定用哪一档写清楚，不同场景口径不同
    scenario = SCENARIOS[scenario_name]
    m_decision = {
        "repo": m1,
        "benchmark": m1_probe,
    }[scenario_name]
    m_decision["target"] = scenario["target"]
    m_decision["met"] = (
        m_decision["value"] is not None and m_decision["value"] >= scenario["target"]
    )

    return {
        "schema_version": 2,
        "scenario": scenario,
        "runs_total": total,
        "evidence_coverage": {
            "stream": total,
            "stream_empty": sum(1 for r in runs if not r["has_stream_evidence"]),
            "log": with_log,
            "log_missing": total - with_log,
            "note": (
                "门禁触发率/误报率依赖日志；验证类指标还要求 stream 非空"
                "（0 字节 = agent 被超时杀掉、产物没落盘）。"
                "log_missing>0 或 stream_empty>0 时分母被缩小，不可直接当作全量结论"
            ),
        },
        "metrics": {
            "verification_command_rate": m1,
            "verification_command_rate_loose": m1_loose,
            "verification_command_rate_probe": m1_probe,
            "gate_trigger_rate": m2,
            "degrade_rate": m3,
            "false_positive_rate": m4,
            "decision_rate": m_decision,
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
    scenario = res["scenario"]
    lines: list[str] = []
    lines.append("# Workx 行为统计采集结果（Issue #117）\n")
    lines.append(f"- run 总数：**{res['runs_total']}**")
    lines.append(f"- 日志证据：{cov['log']} / {cov['stream']}（缺 {cov['log_missing']}）")
    lines.append(
        f"- 轨迹非空：{cov['stream'] - cov['stream_empty']} / {cov['stream']}"
        f"（空 {cov['stream_empty']}"
        f"{'：agent 被超时杀掉、产物没落盘，已从验证类指标分母剔除' if cov['stream_empty'] else ''}）\n"
    )

    lines.append("| 指标 | 值 | 计数 | 分母 | 证据源 |")
    lines.append("| --- | --- | --- | --- | --- |")

    def pct(v: float | None) -> str:
        return "—" if v is None else f"{v * 100:.1f}%"

    labels = {
        "verification_command_rate": "验证命令执行率（strict：可识别运行器）",
        "verification_command_rate_loose": "验证命令执行率（loose：+ 自写测试脚本）",
        "verification_command_rate_probe": "验证命令执行率（probe：+ 一次性探针）",
        "gate_trigger_rate": "门禁触发率",
        "degrade_rate": "降级率",
        "false_positive_rate": "误报率（unavailable 放行）",
    }
    for key, label in labels.items():
        d = m[key]
        lines.append(
            f"| {label} | {pct(d['value'])} | {d['numerator']} | {d['denominator']} | {d['evidence']} |"
        )

    # 三档分别报数（#132）：不合并成一个数；判定档由场景决定
    v = m["decision_rate"]
    lines.append(
        f"\n**判定口径（场景 `{scenario['name']}`，档位 `{scenario['decision_tier']}`，"
        f"目标 ≥ {scenario['target'] * 100:.0f}%）**\n"
    )
    verdict = (
        "✅ 达标"
        if v["met"]
        else ("⚠️ 未达标" if v["value"] is not None else "—")
    )
    lines.append(f"**判定结果：{verdict}**（{v['numerator']}/{v['denominator']} = {pct(v['value'])}）\n")
    lines.append(
        "> 口径分场景（#132 建议动作 a）：\n"
        "> - `repo`（真实工程仓库）→ 判定档 `strict`，≥90% 执行过可识别运行器（#117 原口径）；\n"
        "> - `benchmark`（Terminal-Bench 类空目录做题）→ 判定档 `probe`，"
        "「结束前有过至少一次针对性自检动作」——verifier 测试在独立容器，"
        "agent 拿不到 tests/，跑可识别运行器先天不适用；\n"
        "> - 门禁触发率在这类题上是结构性恒零（无工程标记 → 无 goal），不参与判定。\n"
    )

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


# 「长得像轨迹、其实不是轨迹」的产物命名片段。
# 三条都是 20 题真实跑分时踩出来的，每踩一次 run 总数就翻倍一次。
NON_STREAM_MARKERS = (
    "audit",  # workx-audit.jsonl —— 审计日志
    ".stream.raw.",  # <task>.stream.raw.jsonl —— export_run.py --copy-raw 的轨迹副本
    ".session.",  # <task>.session.jsonl —— export_run.py 产的可导入会话（另一套 schema）
)


def is_stream_file(path: Path) -> bool:
    """判断一个 .jsonl 是不是「轨迹」而不是别的产物。

    ⚠️ 目录递归（`--runs-dir` / 传目录）会把 trial 目录里的**每一份** .jsonl 都收进来，
    而这个目录下一次 run 会落下**四份**文件：

      1. `workx-stream.jsonl`        —— 真轨迹，唯一该被当成 run 的
      2. `workx-audit.jsonl`         —— 审计日志，一条工具调用都没有
      3. `<task>.stream.raw.jsonl`   —— export_run.py `--copy-raw` 拷来的**轨迹副本**
      4. `<task>.session.jsonl`      —— export_run.py 产的可导入会话，**另一套 schema**

    第 2/3/4 类算进来的后果不是「多几条记录」，而是**一次 run 被记成四次**，
    且副本里有两份解析不出任何步骤 → 凭空灌进一批「没跑验证命令」的样本。

    实测踩坑顺序（同一份 20 题产物）：
      · 只排 audit 之前：38 个 run，执行率假报 2.6%
      · 只排 audit 之后：53 个 run，执行率假报 4.4%（raw + session 又混进来）
      · 三类全排之后：**21 个 run**（20 题 job 19 个 + 两次 smoke 各 1 个）

    → 结论：**目录递归必须严格收口**，否则分母失真、判定口径形同虚设。
    """
    name = path.name.lower()
    if not name.endswith(".jsonl"):
        return False
    return not any(m in name for m in NON_STREAM_MARKERS)


def expand_paths(paths: list[str]) -> list[Path]:
    out: list[Path] = []
    for p in paths:
        path = Path(p)
        if path.is_dir():
            out.extend(sorted(f for f in path.rglob("*.jsonl") if is_stream_file(f)))
        else:
            out.append(path)
    return out


def self_test() -> int:
    """用内置样例自检，确认解析链路可用（不是跑分，是协议验证）。"""
    # 护栏：目录递归的收口规则。每次放宽 is_stream_file 都必须同步这里，
    # 否则 20 题实跑会再次把一次 run 记成四次（实测踩过 38 → 53 → 21）。
    ok = True
    for name in (
        "workx-stream.jsonl",
        "workx-audit.jsonl",
        "cobol-modernization.stream.raw.jsonl",
        "cobol-modernization.session.jsonl",
        "regex-log.audit.jsonl",
    ):
        want = name == "workx-stream.jsonl"
        got = is_stream_file(Path(name))
        status = "PASS" if got is want else "FAIL"
        if got is not want:
            ok = False
        print(f"[{status}] is_stream_file({name}) = {got}")

    # #132：命令档位单测——probe 档新增的每类模式都要命中，且不污染 strict
    tier_cases = (
        # (命令, 期望档位)
        ("ctest -R foo", _LEVEL_STRICT),
        ("python3 -m pytest -q", _LEVEL_STRICT),
        ("python3 test_regex.py", _LEVEL_LOOSE),
        ("bash test_diff.sh", _LEVEL_LOOSE),
        ("python3 -c 'import sqlite3; c=sqlite3.connect(\"m.db\"); print(list(c.execute(\"select 1\")))'", _LEVEL_PROBE),
        ("od -c main.db | head -5", _LEVEL_PROBE),
        ("sqlite3 main.db 'select count(*) from t'", _LEVEL_PROBE),
        ("diff out.expected out.actual", _LEVEL_PROBE),
        ("bash probe_diff.sh", _LEVEL_PROBE),
        ("./probe.sh", _LEVEL_PROBE),
        ("./a.out --check", _LEVEL_PROBE),
        ("python3 --version", _LEVEL_NONE),
        ("cmake --version", _LEVEL_NONE),
        ("ls -la", _LEVEL_NONE),
        ("cat README.md", _LEVEL_NONE),
    )
    for cmd, want in tier_cases:
        got = command_verification_level(cmd)
        status = "PASS" if got == want else "FAIL"
        if got != want:
            ok = False
        print(f"[{status}] level({cmd[:40]!r}) = {got}（期望 {want}）")

    fx = Path(__file__).parent / "fixtures"
    files = sorted(fx.glob("*.jsonl"))
    if not files:
        print("FAIL: 缺少 fixtures", file=sys.stderr)
        return 1
    res = collect(files)
    m = res["metrics"]
    # run_d 是回归护栏：只有 `python3 --version` / `cmake --version` 这类问询命令，
    # 严格口径与宽松口径都**不能**算验证命令（2026-10-03 首轮真实跑分踩到的假阳性）。
    # run_e 是 #132 回归护栏：纯一次性探针（python3 -c / od -c / ./ 产物），
    # strict/loose 不算，probe 档必须命中——否则 probe 档形同虚设（首轮两档同分 6.7% 的教训）。
    expect = {
        "verification_command_rate": (2, 5),
        "verification_command_rate_loose": (2, 5),
        "verification_command_rate_probe": (3, 5),
        "gate_trigger_rate": (2, 4),  # run_e 无 .log，不进门禁类分母
        "degrade_rate": (1, 5),
        "false_positive_rate": (0, 4),
    }
    for key, (num, den) in expect.items():
        got = (m[key]["numerator"], m[key]["denominator"])
        status = "PASS" if got == (num, den) else "FAIL"
        if got != (num, den):
            ok = False
        print(f"[{status}] {key}: 期望 {num}/{den}，实得 {got[0]}/{got[1]}")

    # 场景判定：repo 用 strict、benchmark 用 probe（#132 建议动作 a）
    res_bm = collect(files, scenario_name="benchmark")
    check_sc = res_bm["metrics"]["decision_rate"]
    status = "PASS" if check_sc["numerator"] == 3 else "FAIL"
    if check_sc["numerator"] != 3:
        ok = False
    print(f"[{status}] benchmark 场景判定分子 = {check_sc['numerator']}（期望 3）")
    status = "PASS" if res["scenario"]["decision_tier"] == "strict" else "FAIL"
    print(f"[{status}] repo 场景判定档 = {res['scenario']['decision_tier']}")

    print(f"[{'PASS' if ok else 'FAIL'}] self-test 总体")
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Workx 行为统计采集器（Issue #117 / #132）")
    ap.add_argument("paths", nargs="*", help="stream.jsonl 文件或包含它们的目录")
    ap.add_argument("--runs-dir", help="run 产物根目录（递归找 *.jsonl）")
    ap.add_argument("--baseline", help="对照组目录（门禁关闭），用于成本增幅")
    ap.add_argument(
        "--scenario",
        choices=sorted(SCENARIOS),
        default="repo",
        help="判定场景（#132）：repo=真实工程仓库（strict 档）；"
        "benchmark=空目录做题（probe 档，针对性自检动作）",
    )
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

    res = collect(files, scenario_name=args.scenario)
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

    decision = res["metrics"]["decision_rate"]
    return 0 if (decision["value"] is None or decision["met"]) else 3  # 3 = 未达标，供 CI 判定


if __name__ == "__main__":
    raise SystemExit(main())
