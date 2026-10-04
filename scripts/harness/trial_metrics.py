#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Harbor trial 级成绩采集器 —— Issue #142。

背景：Terminal-Bench / Harbor 的一个 trial 无论出了什么事，`reward.txt` 都可能是 0。
**verifier 自己崩了（装不上 uv、连不上 github、超时）也写 0**，于是被统计成
「agent 失败」—— 但 agent 压根没被判定过。

2026-10-04 实测：同一 job 的 6 个 trial 里 3 个属于此类（50%）。
更严重的是回溯首轮 20 题时才发现，其中 3 题同样是 uv 装不上，
也就是说「45.0%」这个数字里混着相当比例的**无效判定**。

本采集器做的事：

  1. 逐个 trial 判定 `verifier_valid`（测试到底有没有真的跑起来）
  2. 同时给出**三档分数**，不再只有一个被污染的数字：
       - pass@1（全部 trial，= harbor 原始口径）
       - pass@1（有效 trial）
       - verifier 失效率  ← 这个数字衡量的是**评测环境本身的可信度**
  3. 逐 task 汇总，专门标出「一次有效判定都没有」的 task（unverifiable）

判定树（顺序敏感，先正后负）

  ctrf.json 存在且解析出 tests >= 1        -> VALID（source=ctrf）
  没有 ctrf 但 pytest 跑出了结论行          -> VALID（source=pytest_summary）
  否则 -> INVALID，并给出 reason：
      verifier_infra       依赖安装失败（uv 下载 / command not found / conn refused）
      verifier_incomplete  verifier 没跑完（自身超时；pytest 起跑但无结论）
      environment_failed   环境容器就没起来（连 verifier 产物都没有）

为什么主判据用 ctrf.json 而不是「reward.txt 存在」
  因为 test.sh 的结构是 `uvx ... pytest --ctrf /logs/verifier/ctrf.json; echo $? > reward.txt`
  —— **`$?` 无论是什么都会写 reward**。所以 reward.txt 的存在与否完全没有区分力，
  而 ctrf.json 只有 pytest 真正跑完才会产出。

用法
  python trial_metrics.py --self-test
  python trial_metrics.py jobs/2026-10-03__13-45-16
  python trial_metrics.py jobs/ -o trials.json --md trials.md
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import tempfile
from pathlib import Path
from typing import Any

CTRF_NAME = "ctrf.json"
REWARD_NAME = "reward.txt"
STDOUT_NAME = "test-stdout.txt"
STDERR_NAME = "test-stderr.txt"

# verifier 侧基础设施故障的文本标记（小写匹配）
# 全部来自 2026-10-03/04 真实跑分日志，不是推测出来的。
INFRA_MARKERS: tuple[tuple[str, str], ...] = (
    ("uv_download", "failed to download https://github.com/astral-sh/uv"),
    ("uv_download", "astral.sh/uv/"),  # 与 URL 同行的错误信息
    ("network", "couldn't connect to server"),
    ("network", "failed to connect to"),
    ("network", "connection refused"),
    ("network", "network is unreachable"),
    ("network", "temporary failure in name resolution"),
    ("missing_dep", "command not found"),
    ("missing_path", "no such file or directory"),
)

#: pytest 结论行，形如 `======= 2 passed, 1 failed in 0.12s =======`
PYTEST_SUMMARY_RE = re.compile(
    r"={3,}\s*\d+.*(?:passed|failed|error|skipped).*in\s+[\d.]+s\s*={3,}"
)
#: 依赖安装失败的显式提示（`uv 的 release process is not working` 等）
UV_INSTALL_HINT = "this may be a standard network error"

#: **环境层**失败标记。它与 verifier 层失败是两回事：
#: 环境层出问题（容器栈起不来 / compose service 解析不了）时 agent 可能压根没跑，
#: 混进「verifier 失效率」会把锅算错地方。日志里这些串是 harbor 自己抛的。
ENV_START_TIMEOUT_MARK = "environment start timed out"
ENV_BROKEN_MARKS = (
    "could not resolve container for compose service",
    "environmentstarttimeouterror",
    "environmentsetuperror",
)

#: trial 目录名形如 `<task>__<trial_id>`；harbor 的 export 目录不带 __
TRIAL_DIR_RE = re.compile(r"^(?P<task>.+)__(?P<tid>[A-Za-z0-9]+)$")


# ---------------------------------------------------------------------------
# 单 trial 判定
# ---------------------------------------------------------------------------


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except (FileNotFoundError, NotADirectoryError, PermissionError):
        return ""


def _read_reward(vdir: Path) -> int | None:
    raw = _read_text(vdir / REWARD_NAME).strip()
    if not raw:
        return None
    try:
        return int(float(raw))
    except ValueError:
        return None


def _read_ctrf(vdir: Path) -> dict[str, Any] | None:
    path = vdir / CTRF_NAME
    if not path.is_file():
        return None
    try:
        obj = json.loads(_read_text(path))
    except json.JSONDecodeError:
        return None
    if not isinstance(obj, dict):
        return None
    summary = obj.get("results", {}).get("summary")
    return summary if isinstance(summary, dict) else None


def _to_int(value: Any) -> int:
    return value if isinstance(value, int) else 0


def judge_verifier(vdir: Path, trial_dir: Path | None = None) -> dict[str, Any]:
    """判定一个 trial 的 verifier 到底有没有真的跑完测试。

    返回 dict，核心字段：
      verifier_valid : bool
      reason         : "" 表示有效，否则是机器可读的失效原因
      detail         : 命中的具体文本（用于报告里举证）
      source         : ctrf / pytest_summary / none
    """
    reward = _read_reward(vdir)
    summary = _read_ctrf(vdir)
    stdout = _read_text(vdir / STDOUT_NAME)
    stderr = _read_text(vdir / STDERR_NAME)
    blob = (stdout + "\n" + stderr).lower()
    trial_log = _read_text(trial_dir / "trial.log") if trial_dir else ""
    exception_txt = _read_text(trial_dir / "exception.txt") if trial_dir else ""

    out: dict[str, Any] = {
        "reward": reward,
        "reward_present": reward is not None,
        "verifier_valid": False,
        "reason": "",
        "detail": "",
        "source": "none",
        "ctrf_tests": None,
        "ctrf_passed": None,
        "ctrf_failed": None,
        "has_ctrf": summary is not None,
    }

    # --- 正判据 1：ctrf.json 存在且确实跑了测试 ---
    if summary is not None:
        tests = _to_int(summary.get("tests"))
        passed = _to_int(summary.get("passed"))
        failed = _to_int(summary.get("failed"))
        out.update(ctrf_tests=tests, ctrf_passed=passed, ctrf_failed=failed)
        if tests >= 1:
            out.update(verifier_valid=True, source="ctrf")
            return out
        out.update(reason="ctrf_zero_tests", detail=f"tests={tests}")
        return out

    # --- 正判据 2：没有 ctrf，但 pytest 跑到了结论行 ---
    if PYTEST_SUMMARY_RE.search(stdout):
        out.update(verifier_valid=True, source="pytest_summary")
        return out

    # --- 负判据：逐个归因 ---
    for reason, marker in INFRA_MARKERS:
        idx = blob.find(marker)
        if idx >= 0:
            line_start = max(0, blob.rfind("\n", 0, idx) + 1)
            line_end = blob.find("\n", idx)
            line = stdout[max(0, line_start):(line_end if line_end >= 0 else len(stdout))]
            out.update(reason=f"verifier_infra:{reason}", detail=line.strip()[:200])
            return out
    if UV_INSTALL_HINT in blob:
        out.update(reason="verifier_infra:uv_install", detail="uv installer printed network hint")
        return out

    # --- 环境层失败（先于 verifier 层判定，否则会被误归成 verifier 超时）---
    harness_text = ((trial_log if trial_dir else "") + "\n" + exception_txt).lower()
    if ENV_START_TIMEOUT_MARK in harness_text:
        out.update(reason="environment_start_timeout", detail="environment did not start in time")
        return out
    for mark in ENV_BROKEN_MARKS:
        if mark in harness_text:
            out.update(reason="environment_failed", detail=mark)
            return out

    if "verifier execution timed out" in harness_text:
        out.update(reason="verifier_incomplete:timeout", detail="verifier exceeded its timeout")
        return out
    if "timed out" in trial_log.lower():
        out.update(reason="verifier_incomplete:timeout", detail="harness reported a timeout")
        return out
    if "test session starts" in stdout:
        out.update(
            reason="verifier_incomplete:no_summary",
            detail="pytest started but never reached a summary line",
        )
        return out
    if not stdout.strip() and exception_txt.strip():
        out.update(
            reason="environment_failed",
            detail=exception_txt.strip().splitlines()[-1][:200],
        )
        return out

    if not stdout.strip():
        out.update(
            reason="verifier_incomplete:empty_output",
            detail="no ctrf, no test-stdout",
        )
        return out

    # 没能归因：不硬塞进 verifier/environment，标成 unknown 强迫人工看日志，
    # 免得「不知道为什么」被统计成「verifier 坏了」。
    out.update(reason="unknown_no_marker", detail="no ctrf and no known failure marker")
    return out


def reason_category(reason: str) -> str:
    """把失效原因归到三类，便于区分「谁的问题」。

    verifier     verifier 自己的依赖/执行失败 → **是评测环境的问题**
    environment  环境层面失败（容器栈起不来）→ agent 可能根本没跑
    unknown      没匹配到已知标记，需要人工看日志
    """
    if not reason:
        return "ok"
    if reason.startswith(("verifier_infra", "verifier_incomplete", "verifier_invalid",
                          "ctrf_zero_tests")):
        return "verifier"
    if reason.startswith("environment"):
        return "environment"
    return "unknown"


def parse_trial(trial_dir: Path) -> dict[str, Any]:
    m = TRIAL_DIR_RE.match(trial_dir.name)
    vdir = trial_dir / "verifier"
    verdict = judge_verifier(vdir, trial_dir)
    return {
        "trial": trial_dir.name,
        "job": trial_dir.parent.name,
        "task": m.group("task") if m else trial_dir.name,
        "trial_id": m.group("tid") if m else "",
        "path": str(trial_dir),
        "category": reason_category(verdict["reason"]),
        **verdict,
    }


# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------


def discover_trials(roots: list[Path]) -> list[Path]:
    """从给定的根向下找 trial 目录，最多下探两层。

    Harbor 的产物是两层结构：``jobs/<job-时间戳>/<task>__<trial_id>/``，
    而用户也可能直接传入某个 job 目录或某一个 trial 目录，这里三种都要能识别。
    """
    found: list[Path] = []
    for root in roots:
        if not root.is_dir():
            continue
        if TRIAL_DIR_RE.match(root.name):
            found.append(root)
            continue
        for child in sorted(root.iterdir()):
            if not child.is_dir():
                continue
            if TRIAL_DIR_RE.match(child.name):
                found.append(child)
                continue
            # 第二层：job 目录下才是 trial（详见 docstring）
            for grand in sorted(child.iterdir()):
                if grand.is_dir() and TRIAL_DIR_RE.match(grand.name):
                    found.append(grand)
    return found


def _ratio(hit: int, total: int) -> dict[str, Any]:
    return {
        "numerator": hit,
        "denominator": total,
        "value": (hit / total) if total else None,
    }


def collect_trials(trial_dirs: list[Path]) -> dict[str, Any]:
    trials = [parse_trial(d) for d in trial_dirs]
    trials.sort(key=lambda t: (t["job"], t["task"], t["trial_id"]))

    have_reward = [t for t in trials if t["reward_present"]]
    valid = [t for t in trials if t["verifier_valid"]]
    passes_all = [t for t in have_reward if t["reward"] == 1]
    passes_valid = [t for t in valid if t["reward"] == 1]

    # --- 逐 task 汇总 ---
    by_task: dict[str, dict[str, Any]] = {}
    for t in trials:
        key = f'{t["job"]}::{t["task"]}' if len({x["job"] for x in trials}) > 1 else t["task"]
        entry = by_task.setdefault(
            key,
            {"task": t["task"], "job": t["job"], "attempts": 0, "valid_attempts": 0,
             "pass_all": 0, "pass_valid": 0, "reasons": [], "trials": []},
        )
        entry["attempts"] += 1
        entry["trials"].append(t["trial"])
        if t["verifier_valid"]:
            entry["valid_attempts"] += 1
            if t["reward"] == 1:
                entry["pass_valid"] += 1
        if t["reward"] == 1:
            entry["pass_all"] += 1

    tasks_out = []
    for entry in by_task.values():
        if entry["valid_attempts"] >= 1:
            verdict = "pass" if entry["pass_valid"] >= 1 else "fail"
        else:
            verdict = "unverifiable"
        tasks_out.append(
            {
                "task": entry["task"],
                "job": entry["job"],
                "attempts": entry["attempts"],
                "valid_attempts": entry["valid_attempts"],
                "pass_all": entry["pass_all"],
                "pass_valid": entry["pass_valid"],
                "verdict": verdict,
                "trials": entry["trials"],
            }
        )
    tasks_out.sort(key=lambda x: (x["job"], x["task"]))

    counted = [t for t in tasks_out if t["verdict"] != "unverifiable"]
    passed_tasks = [t for t in tasks_out if t["verdict"] == "pass"]

    # ⚠️ pass_rate_all 的分母必须是 **全部 trial**，而不是「有 reward.txt 的 trial」。
    #    因为 harbor 把 reward.txt 缺失的 trial 也当 0 分算进分母（9/20 = 45.0% 就是这么来的）。
    #    只有口径完全一致，「45.0% → 60.0%」这个修正才站得住。
    return {
        "schema_version": 1,
        "trials_total": len(trials),
        "trials_with_reward": len(have_reward),
        "trials_valid": len(valid),
        "verifier_failure_rate": (
            (len(trials) - len(valid)) / len(trials) if trials else None
        ),
        # 失效原因按「谁的锅」分类，避免把环境层故障算到 verifier 头上
        "reason_breakdown": {
            cat: sum(1 for t in trials if t["category"] == cat)
            for cat in ("verifier", "environment", "unknown")
        },
        "pass_rate_all": _ratio(len(passes_all), len(trials)),
        "pass_rate_valid": _ratio(len(passes_valid), len(valid)),
        "task_pass_rate_valid": _ratio(len(passed_tasks), len(counted)),
        "tasks_total": len(tasks_out),
        "tasks_unverifiable": len([t for t in tasks_out if t["verdict"] == "unverifiable"]),
        "invalid_trials": [
            {"trial": t["trial"], "reason": t["reason"], "category": t["category"],
             "detail": t["detail"]}
            for t in trials
            if not t["verifier_valid"]
        ],
        "tasks": tasks_out,
        "per_trial": trials,
    }


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------


def _pct(v: float | None) -> str:
    return "—" if v is None else f"{v * 100:.1f}%"


def to_markdown(res: dict[str, Any]) -> str:
    lines: list[str] = []
    lines.append("# Harbor trial 成绩采集结果（Issue #142）\n")
    jobs_seen = len({t["job"] for t in res["tasks"]})
    lines.append(
        f"- trial 数：**{res['trials_total']}** / task 数：**{res['tasks_total']}**"
        f" / job 数：**{jobs_seen}**"
    )
    lines.append("")

    lines.append("| 口径 | 值 | 计数 | 分母 |")
    lines.append("| --- | --- | --- | --- |")
    pa, pv, tp = res["pass_rate_all"], res["pass_rate_valid"], res["task_pass_rate_valid"]
    lines.append(f"| pass@1（全部 trial，= harbor 原始口径） | {_pct(pa['value'])} | {pa['numerator']} | {pa['denominator']} |")
    lines.append(f"| **pass@1（有效 trial）** | **{_pct(pv['value'])}** | {pv['numerator']} | {pv['denominator']} |")
    lines.append(f"| pass@1（task 级，有有效判定的题） | {_pct(tp['value'])} | {tp['numerator']} | {tp['denominator']} |")
    lines.append(
        f"| **无有效判定率** | **{_pct(res['verifier_failure_rate'])}** "
        f"| {res['trials_total'] - res['trials_valid']} | {res['trials_total']} |"
    )
    lines.append("")

    # ⚠️ 「无有效判定」≠「verifier 坏了」。环境层故障（容器栈起不来）时 agent 可能压根没跑，
    #    把它算到 verifier 头上会让诊断跑偏，所以这里强制拆开归因。
    bd = res["reason_breakdown"]
    lines.append("### 失败归因：到底是谁的问题\n")
    lines.append("| 类别 | 含义 | trial 数 | 占比 |")
    lines.append("| --- | --- | --- | --- |")
    labels = {
        "verifier": "verifier 自己的依赖装不上 / 没跑完（**评测环境的问题**，与 agent 无关）",
        "environment": "容器栈起不来 / 启动超时（**agent 可能根本没跑**）",
        "unknown": "没匹配到已知标记（**需人工看日志**）",
    }
    for cat in ("verifier", "environment", "unknown"):
        n = bd.get(cat, 0)
        lines.append(
            f"| `{cat}` | {labels[cat]} | {n} | {_pct(n / res['trials_total'])
            if res['trials_total'] else '—'} |"
        )
    ok_n = res["trials_valid"]
    lines.append(
        f"| `ok` | verifier 真的跑完了并对给分 | {ok_n} | "
        f"{_pct(ok_n / res['trials_total']) if res['trials_total'] else '—'} |"
    )
    lines.append("")

    if res["tasks_unverifiable"]:
        lines.append(
            f"> ⚠️ 有 **{res['tasks_unverifiable']}** 个 task **一次有效判定都没有**，"
            "它的成绩既不能算通过也不能算失败 —— 只能在报告里记为 unverifiable。\n"
        )

    if res["invalid_trials"]:
        lines.append("## 无效 trial 明细\n")
        lines.append("| trial | 失效原因 | 证据 |")
        lines.append("| --- | --- | --- |")
        for item in res["invalid_trials"]:
            detail = item["detail"].replace("|", "\\|")
            lines.append(f"| {item['trial']} | `{item['reason']}` | {detail[:80]} |")
        lines.append("")

    lines.append("## 逐 task 汇总\n")
    multi_job = len({t["job"] for t in res["tasks"]}) > 1
    if multi_job:
        lines.append("| job | task | verdict | 通过/有效判定 | 有效/总 attempt |")
        lines.append("| --- | --- | --- | --- | --- |")
    else:
        lines.append("| task | verdict | 通过/有效判定 | 有效/总 attempt |")
        lines.append("| --- | --- | --- | --- |")
    icon = {"pass": "✅", "fail": "❌", "unverifiable": "⚪"}
    for task in res["tasks"]:
        cells = [
            f"{icon.get(task['verdict'], '')} {task['verdict']}",
            f"{task['pass_valid']}/{task['valid_attempts']}",
            f"{task['valid_attempts']}/{task['attempts']}",
        ]
        prefix = f"| {task['job']} | {task['task']} " if multi_job else f"| {task['task']} "
        lines.append(prefix + " | " + " | ".join(cells) + " |")
    lines.append("")

    return "\n".join(lines) + "\n"


# ---------------------------------------------------------------------------
# 自检：合成 trial 目录树（不入库，跑在临时目录里）
# ---------------------------------------------------------------------------

_CTRF_OK = json.dumps(
    {"results": {"summary": {"tests": 3, "passed": 3, "failed": 0}}}, ensure_ascii=False
)
_CTRF_FAIL = json.dumps(
    {"results": {"summary": {"tests": 3, "passed": 1, "failed": 2}}}, ensure_ascii=False
)
_UV_CRASH = "\n".join(
    [
        "downloading uv 0.9.5 x86_64-unknown-linux-gnu",
        "curl: (7) Failed to connect to github.com port 443 after 21081 ms: Couldn't connect to server",
        "failed to download https://github.com/astral-sh/uv/releases/download/0.9.5/uv-x86_64-unknown-linux-gnu.tar.gz",
        "this may be a standard network error, but it may also indicate",
        "/tests/test.sh: line 19: uvx: command not found",
    ]
)
_TIMEOUT_STDOUT = "\n".join(
    [
        "Installed 24 packages in 30ms",
        "============================= test session starts ==============================",
        "collected 2 items",
        "../tests/test_outputs.py F",
    ]
)


def _write(base: Path, rel: str, content: str) -> None:
    path = base / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")


def _build_fixture_tree(root: Path) -> None:
    """合成 6 个 trial，覆盖判定树的每条分支。"""
    cases = {
        # 目录尾名: (reward, ctrf|None, stdout, trial_log|None, exception|None)
        "good__AAA111": ("1", _CTRF_OK, "ok", None, None),
        "failed__BBB222": ("0", _CTRF_FAIL, "ok", None, None),
        "uvcrash__CCC333": ("0", None, _UV_CRASH, None, None),
        "timeouter__DDD444": (None, None, _TIMEOUT_STDOUT,
                              "Trial failed: Verifier execution timed out after 900.0 seconds", None),
        "norun__EEE555": (None, None, "", None,
                          "RuntimeError: Could not resolve container for compose service 'main'."),
        "weird__FFF666": ("0", None, "some random output", None, None),
    }
    for name, (reward, ctrf, stdout, tlog, exc) in cases.items():
        d = root / name
        _write(d, f"verifier/{REWARD_NAME}", reward if reward is not None else "")
        if ctrf is not None:
            _write(d, f"verifier/{CTRF_NAME}", ctrf)
        _write(d, f"verifier/{STDOUT_NAME}", stdout)
        if tlog is not None:
            _write(d, "trial.log", tlog)
        if exc is not None:
            _write(d, "exception.txt", exc)


def self_test() -> int:
    ok = True

    def check(label: str, got: Any, want: Any) -> None:
        nonlocal ok
        status = "PASS" if got == want else "FAIL"
        if got != want:
            ok = False
        print(f"[{status}] {label}: 期望 {want}，实得 {got}")

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        _build_fixture_tree(root)
        trials = {t["trial"]: t for t in (parse_trial(d) for d in discover_trials([root]))}
        names = sorted(trials)
        check("发现 trial 数", len(names), 6)

        check("good 有效", trials["good__AAA111"]["verifier_valid"], True)
        check("good source", trials["good__AAA111"]["source"], "ctrf")
        check("failed 有效", trials["failed__BBB222"]["verifier_valid"], True)
        check("failed ctrf_passed", trials["failed__BBB222"]["ctrf_passed"], 1)
        check("uvcrash 无效", trials["uvcrash__CCC333"]["verifier_valid"], False)
        check("uvcrash reason", trials["uvcrash__CCC333"]["reason"], "verifier_infra:uv_download")
        check("timeouter 无效", trials["timeouter__DDD444"]["verifier_valid"], False)
        check("timeouter reason", trials["timeouter__DDD444"]["reason"],
              "verifier_incomplete:timeout")
        check("norun 无效", trials["norun__EEE555"]["verifier_valid"], False)
        check("norun reason", trials["norun__EEE555"]["reason"], "environment_failed")
        check("weird 无效", trials["weird__FFF666"]["verifier_valid"], False)
        check("weird reason", trials["weird__FFF666"]["reason"], "unknown_no_marker")

        res = collect_trials(discover_trials([root]))
        # harbor 口径：分母是**全部 trial**（缺 reward.txt 也算失败）→ 1/6
        check("pass_rate_all 分子", res["pass_rate_all"]["numerator"], 1)
        check("pass_rate_all 分母", res["pass_rate_all"]["denominator"], 6)
        # 有效的：good / failed = 2，其中 good 通过
        check("pass_rate_valid 分子", res["pass_rate_valid"]["numerator"], 1)
        check("pass_rate_valid 分母", res["pass_rate_valid"]["denominator"], 2)
        check("无效 trial 数", len(res["invalid_trials"]), 4)
        # 归因分类：2 个 verifier 层、1 个环境层、1 个 unknown
        check("category=verifier", res["reason_breakdown"]["verifier"], 2)
        check("category=environment", res["reason_breakdown"]["environment"], 1)
        check("category=unknown", res["reason_breakdown"]["unknown"], 1)
        # 6 个 task 各 1 次 attempt；4 个 unverifiable
        check("task 数", res["tasks_total"], 6)
        check("unverifiable task 数", res["tasks_unverifiable"], 4)
        check("task 级通过数", res["task_pass_rate_valid"]["numerator"], 1)

        # pytest 结论行但没有 ctrf 的情况（少见，但是正判据的兜底）
        extra = root.parent / "extra"
        d = root / "summaryonly__GGG777"
        _write(d, f"verifier/{REWARD_NAME}", "0")
        _write(d, f"verifier/{STDOUT_NAME}",
               "=========== 1 passed, 2 failed in 0.12s ============")
        check("无 ctrf 但有结论行 -> 有效", parse_trial(d)["verifier_valid"], True)
        check("来源标记", parse_trial(d)["source"], "pytest_summary")
        del extra

    print(f"[{'PASS' if ok else 'FAIL'}] self-test 总体")
    return 0 if ok else 1


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description="Harbor trial 级成绩采集器（Issue #142）")
    ap.add_argument("paths", nargs="*", help="job 目录，或直接是 trial 目录")
    ap.add_argument("-o", "--out", help="输出 JSON 路径")
    ap.add_argument("--md", help="输出 Markdown 路径")
    ap.add_argument("--self-test", action="store_true", help="合成样例自检")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.paths:
        ap.error("需要至少一个路径，或用 --self-test 自检")

    roots = [Path(p) for p in args.paths]
    missing = [str(p) for p in roots if not p.exists()]
    if missing:
        print(f"路径不存在：{missing}", file=sys.stderr)
        return 1

    dirs = discover_trials(roots)
    if not dirs:
        print("未找到任何 trial 目录（形如 <task>__<trial_id>）", file=sys.stderr)
        return 1

    res = collect_trials(dirs)
    text = json.dumps(res, ensure_ascii=False, indent=2)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"已写入 {args.out}")
    md = to_markdown(res)
    if args.md:
        Path(args.md).write_text(md, encoding="utf-8")
        print(f"已写入 {args.md}")
    if not args.out and not args.md:
        print(md)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
