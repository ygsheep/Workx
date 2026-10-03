#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Harbor 跑分产物导出器 —— 把一整轮跑分变成「人能读」和「工具能看」两份东西。

背景
  Harbor 跑完会在 `jobs/<job-id>/<task>__<id>/` 下留下四份产物，其中
  `artifacts/tmp/workx-stream.jsonl`（--artifact 下载回来的）是逐步轨迹，
  末行是 `result`（含 usage）。但它是**评测用的 schema**（step_number/type/...），
  既不好直接读，也不是 `~/.workx/projects/` 下会话查看工具认的格式。

产出
  1. `<task>.transcript.md`       可读轨迹：thought → action → observation 逐步展开，
                                  附工具调用序列、最终答复、verifier 输出
  2. `<task>.session.jsonl`       workx 原生会话格式，可放进
                                  `<config_dir>/projects/<name>/<session_id>.jsonl`
                                  供本地会话查看工具直接渲染

两套 schema 的对应关系
    stream-json                    →  原生会话
    thought (text)                 →  assistant{content}
    action (tool_name/tool_input)  →  assistant{toolUses:[{id,name,input}]}
    observation (observation)      →  tool{toolName,content,isError,toolCallId}
    final_answer (text)            →  assistant{content}
    另补 session_start / title / user(任务原文) / session_end

⚠️ 工具名要从**上一个 action** 带到紧随其后的 observation 上：stream 的
   observation 行本身不带 tool_name，漏了这条会话里所有 tool 消息的 toolName 都是空的。

⚠️ 时间戳：stream 里没有时间字段。按 job 目录名（YYYY-MM-DD__HH-MM-SS，本地时间）
   解析起点、把各步均匀铺在实测 duration_ms 上，让时间轴节奏接近真实。
   解析不到就回落到 stream 文件的 mtime。

用法
  # 导出整个 job（默认写到 jobs/<job-id>/export/）
  python scripts/harness/export_run.py --runs-dir jobs/2026-10-03__11-45-52

  # 顺手装进 ~/.workx/projects/<前缀>-<题名>/ 给本地工具看
  python scripts/harness/export_run.py --runs-dir jobs/<job-id> --to-workx-projects

  python scripts/harness/export_run.py --self-test    # 跑内置样例自检
"""

from __future__ import annotations

import argparse
import json
import sys
import uuid
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

# 单个 observation 超过这个长度就截断，避免一道题的轨迹把 Markdown 撑到几 MB
MAX_OBSERVATION = 4000

# Harbor 会把数据集缓存到 ~/.cache/harbor/tasks/<hash>/<task>/instruction.md，
# 任务原文从这里取；取不到就退化成占位说明（不伪造内容）
DEFAULT_TASKSET_ROOT = Path.home() / ".cache" / "harbor" / "tasks"


# ============================================================================
# 基础工具
# ============================================================================


def fence(text: str, lang: str = "") -> str:
    """用 4 反引号包裹代码块 —— 内容里的 ``` 会破坏 Markdown，3 个不够。"""
    body = text if text.endswith("\n") else text + "\n"
    return f"````{lang}\n{body}\n````"


def iso_utc(dt: datetime) -> str:
    """对齐 src/agent/session/session_store.cpp 的 now_iso()：%Y-%m-%dT%H:%M:%SZ"""
    return dt.astimezone(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def encode_project_name(name: str) -> str:
    """对齐 src/core/utils/path_encoder.cpp：只有 \\ / : 会被换成 -。"""
    for ch in ("\\", "/", ":"):
        name = name.replace(ch, "-")
    return name


def find_streams(runs_dir: Path) -> list[Path]:
    """一个 job 目录里可能有多道题 × 多个 trial，全部找出来。"""
    return sorted(runs_dir.glob("*/artifacts/tmp/workx-stream.jsonl"))


def trial_dir_of(stream: Path) -> Path:
    """artifacts/tmp/workx-stream.jsonl → <trial 目录>"""
    return stream.parents[2]


def task_name_of(trial_dir: Path) -> str:
    """<task>__<id> → <task>"""
    return trial_dir.name.split("__")[0]


def find_instruction(task: str, root: Path) -> str:
    """从 Harbor 数据集缓存里取任务原文；取不到返回占位说明。"""
    for p in sorted(root.glob(f"*/{task}/instruction.md")):
        return p.read_text(encoding="utf-8")
    return f"（未能从 {root} 找到 {task}/instruction.md，任务原文缺失）"


def job_start_utc(runs_dir: Path, stream: Path) -> datetime:
    """优先按 job 目录名解析起点，回落 stream 文件 mtime。"""
    for part in (runs_dir.name, runs_dir.parent.name):
        try:
            naive = datetime.strptime(part, "%Y-%m-%d__%H-%M-%S")
        except ValueError:
            continue
        # Harbor 的 job 目录名用的是**本地时间**，按本机时区解释后再转 UTC
        return naive.replace(tzinfo=None).astimezone()
    return datetime.fromtimestamp(stream.stat().st_mtime, tz=timezone.utc).astimezone()


def read_reward(trial_dir: Path) -> str:
    p = trial_dir / "verifier" / "reward.txt"
    return p.read_text(encoding="utf-8").strip() if p.exists() else ""


def read_verifier_stdout(trial_dir: Path) -> str:
    p = trial_dir / "verifier" / "test-stdout.txt"
    if not p.exists():
        return ""
    return p.read_text(encoding="utf-8", errors="replace").strip()[-1500:]


# ============================================================================
# 产出 1：可读轨迹
# ============================================================================


def write_transcript(stream: Path, out_path: Path, task: str) -> dict[str, Any]:
    lines = [json.loads(l) for l in stream.read_text(encoding="utf-8").splitlines() if l.strip()]
    steps, result = lines[:-1], lines[-1]
    trial = trial_dir_of(stream)
    usage = result.get("usage") or {}

    reward = read_reward(trial)
    vout = read_verifier_stdout(trial)

    with out_path.open("w", encoding="utf-8") as f:
        f.write(f"# {task} · 运行轨迹\n\n")
        f.write("| 项 | 值 |\n| --- | --- |\n")
        f.write(f"| job | `{trial.parent.name}` |\n")
        f.write(f"| trial | `{trial.name}` |\n")
        f.write(f"| session_id | `{result.get('session_id', '')}` |\n")
        f.write(f"| reward | **{reward or 'n/a'}** |\n")
        f.write(f"| goal_status | {result.get('goal_status')} |\n")
        f.write(f"| was_error / was_interrupted "
                f"| {result.get('was_error')} / {result.get('was_interrupted')} |\n")
        f.write(f"| prompt_tokens | {usage.get('prompt_tokens')} |\n")
        f.write(f"| generated_tokens | {usage.get('generated_tokens')} |\n")
        f.write(f"| iterations / tool_calls "
                f"| {usage.get('total_iterations')} / {usage.get('total_tool_calls')} |\n")
        f.write(f"| duration | {usage.get('duration_ms', 0) / 1000.0:.1f} s |\n")
        f.write("\n")

        f.write(f"## 轨迹（{len(steps)} 步）\n\n")
        tool_seq: list[str] = []
        for s in steps:
            kind, n = s.get("type"), s.get("step_number")
            if kind == "thought":
                f.write(f"### Step {n} · thought\n\n{s.get('text', '')}\n\n")
            elif kind == "action":
                name = s.get("tool_name", "")
                tool_seq.append(name)
                f.write(f"### Step {n} · action → `{name}`\n\n")
                f.write(fence(json.dumps(s.get("tool_input"), ensure_ascii=False, indent=2), "json"))
                f.write("\n\n")
            elif kind == "observation":
                obs = s.get("observation", "")
                cut = ""
                if len(obs) > MAX_OBSERVATION:
                    obs = obs[:MAX_OBSERVATION]
                    cut = f"\n…（截断，原文共 {len(s.get('observation', ''))} 字符）"
                flag = " ❌ 出错" if s.get("is_error") else ""
                f.write(f"### Step {n} · observation{flag}\n\n")
                f.write(fence(obs + cut))
                f.write("\n\n")
            elif kind == "final_answer":
                f.write(f"### Step {n} · final_answer\n\n{s.get('text', '')}\n\n")

        f.write("## 工具调用序列\n\n")
        f.write(" → ".join(f"`{t}`" for t in tool_seq if t) + "\n\n")
        f.write(f"## 最终答复\n\n{result.get('result', '')}\n\n")
        if vout:
            f.write("## verifier 输出\n\n")
            f.write(fence(vout))
            f.write("\n")

    return {"steps": len(steps), "tools": tool_seq, "reward": reward}


# ============================================================================
# 产出 2：workx 原生会话
# ============================================================================


def build_session(stream: Path, task: str, instruction: str, model: str, cwd: str,
                  start: datetime) -> tuple[list[dict[str, Any]], str]:
    lines = [json.loads(l) for l in stream.read_text(encoding="utf-8").splitlines() if l.strip()]
    steps, result = lines[:-1], lines[-1]
    sid = result.get("session_id", "")
    dur_ms = (result.get("usage") or {}).get("duration_ms", 0)
    span = timedelta(milliseconds=dur_ms) / max(1, len(steps))

    out: list[dict[str, Any]] = []
    out.append({"type": "session_start", "sessionId": sid, "cwd": cwd, "model": model,
                "gitBranch": "", "createdAt": iso_utc(start)})
    out.append({"type": "title", "sessionId": sid, "timestamp": iso_utc(start),
                "title": f"Terminal-Bench 2.0 · {task}（Harbor 跑分）"})

    parent = ""
    now = start

    def emit(obj: dict[str, Any]) -> str:
        nonlocal parent
        u = str(uuid.uuid4())
        obj["uuid"] = u
        obj["parentUuid"] = parent
        obj["timestamp"] = iso_utc(now)
        out.append(obj)
        parent = u
        return u

    emit({"type": "user", "content": instruction})

    pending_id = ""
    pending_name = ""
    for s in steps:
        now += span
        kind = s.get("type")
        if kind == "thought":
            emit({"type": "assistant", "content": s.get("text", ""),
                  "reasoningContent": "", "reasoningMs": 0, "toolUses": []})
        elif kind == "action":
            pending_id = "toolu_" + uuid.uuid4().hex[:12]
            pending_name = s.get("tool_name", "")
            emit({"type": "assistant", "content": "", "reasoningContent": "", "reasoningMs": 0,
                  "toolUses": [{"id": pending_id, "name": pending_name,
                                "input": s.get("tool_input", {})}]})
        elif kind == "observation":
            emit({"type": "tool", "toolCallId": pending_id, "toolName": pending_name,
                  "content": s.get("observation", ""), "isError": bool(s.get("is_error"))})
            pending_id = ""
            pending_name = ""
        elif kind == "final_answer":
            emit({"type": "assistant", "content": s.get("text", ""),
                  "reasoningContent": "", "reasoningMs": 0, "toolUses": []})

    now += span
    out.append({"type": "session_end", "sessionId": sid, "endedAt": iso_utc(now)})
    return out, sid


def write_session(stream: Path, out_path: Path, task: str, instruction: str, model: str,
                  cwd: str, start: datetime) -> tuple[int, str]:
    rows, sid = build_session(stream, task, instruction, model, cwd, start)
    with out_path.open("w", encoding="utf-8") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    return len(rows), sid


# ============================================================================
# 驱动
# ============================================================================


def export_one(stream: Path, outdir: Path, taskset_root: Path, model: str, cwd: str,
               copy_raw: bool) -> dict[str, Any]:
    trial = trial_dir_of(stream)
    task = task_name_of(trial)
    instruction = find_instruction(task, taskset_root)
    start = job_start_utc(trial.parent, stream)

    outdir.mkdir(parents=True, exist_ok=True)
    md_path = outdir / f"{task}.transcript.md"
    sess_path = outdir / f"{task}.session.jsonl"

    info = write_transcript(stream, md_path, task)
    nrows, sid = write_session(stream, sess_path, task, instruction, model, cwd, start)

    if copy_raw:
        raw_dir = outdir / "raw"
        raw_dir.mkdir(exist_ok=True)
        (raw_dir / f"{task}.stream.raw.jsonl").write_bytes(stream.read_bytes())
        for name in ("workx-run.log", "workx-audit.jsonl"):
            src = stream.parent / name
            if src.exists():
                (raw_dir / f"{task}.{name}").write_bytes(src.read_bytes())

    info.update(task=task, session_id=sid, session_rows=nrows,
                transcript=str(md_path), session=str(sess_path))
    return info


def install_to_projects(session_file: Path, task: str, session_id: str, config_dir: Path,
                        prefix: str) -> Path:
    """把会话 jsonl 放进 ~/.workx/projects/<前缀>-<题名>/<session_id>.jsonl

    目录名与文件名的约定见 src/agent/session/session_store.cpp 与
    src/core/utils/path_encoder.cpp。
    """
    proj = config_dir / "projects" / encode_project_name(f"{prefix}-{task}")
    proj.mkdir(parents=True, exist_ok=True)
    dst = proj / f"{session_id}.jsonl"
    dst.write_bytes(session_file.read_bytes())
    return dst


def self_test() -> int:
    """内置样例自检：确认两套 schema 的转换不丢步、工具名不丢。"""
    import tempfile

    stream = [
        {"step_number": 1, "type": "thought", "text": "先看看环境"},
        {"step_number": 2, "type": "action", "tool_name": "Bash",
         "tool_input": {"command": "python3 --version"}},
        {"step_number": 3, "type": "observation", "observation": "Python 3.13.9",
         "is_error": False},
        {"step_number": 4, "type": "action", "tool_name": "Write",
         "tool_input": {"path": "/app/a.txt"}},
        {"step_number": 5, "type": "observation", "observation": "boom", "is_error": True},
        {"step_number": 6, "type": "final_answer", "text": "done"},
        {"result": "done", "session_id": "test-session", "goal_status": 0, "was_error": False,
         "was_interrupted": False,
         "usage": {"prompt_tokens": 100, "generated_tokens": 10, "total_iterations": 3,
                   "total_tool_calls": 2, "duration_ms": 6000}},
    ]
    def _chain_ok(rows: list[dict]) -> bool:
        """只检查带 uuid 的消息（session_start / session_end 是元信息，不参与链）。"""
        linked = [r for r in rows if "uuid" in r]
        if not linked or linked[0].get("parentUuid", "") != "":
            return False
        return all(linked[i]["parentUuid"] == linked[i - 1]["uuid"]
                   for i in range(1, len(linked)))

    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        trial = root / "2026-10-03__00-00-00" / "sample-task__ABC" / "artifacts" / "tmp"
        trial.mkdir(parents=True)
        sp = trial / "workx-stream.jsonl"
        sp.write_text("".join(json.dumps(x, ensure_ascii=False) + "\n" for x in stream),
                      encoding="utf-8")
        # ⚠️ verifier 目录挂在 **trial 根** 下，不在 artifacts 下：
        #    trial_dir_of() = stream.parents[2] = <job>/<task>__<id>
        (sp.parents[2] / "verifier").mkdir()
        (sp.parents[2] / "verifier" / "reward.txt").write_text("1\n", encoding="utf-8")
        (sp.parents[2] / "verifier" / "test-stdout.txt").write_text("ok\n", encoding="utf-8")

        out = root / "export"
        info = export_one(sp, out, DEFAULT_TASKSET_ROOT, "test-model", "/app", copy_raw=False)
        rows = [json.loads(l) for l in (out / "sample-task.session.jsonl")
                .read_text(encoding="utf-8").splitlines() if l.strip()]

        checks = [
            ("轨迹 Markdown 生成", (out / "sample-task.transcript.md").exists()),
            ("会话行数 = 6 步 + 4 条元信息", len(rows) == 10),
            ("首行 session_start", rows[0]["type"] == "session_start"),
            ("末行 session_end", rows[-1]["type"] == "session_end"),
            ("user 消息存在", any(r["type"] == "user" for r in rows)),
            ("tool 消息 2 条", sum(1 for r in rows if r["type"] == "tool") == 2),
            ("toolName 带过来了", [r["toolName"] for r in rows if r["type"] == "tool"]
             == ["Bash", "Write"]),
            ("isError 传递正确", [r["isError"] for r in rows if r["type"] == "tool"]
             == [False, True]),
            ("toolCallId 非空", all(r["toolCallId"] for r in rows if r["type"] == "tool")),
            ("parentUuid 串成链", _chain_ok(rows)),
            ("reward 读到 1", info["reward"] == "1"),
            ("verifier stdout 读到", "ok" in (out / "sample-task.transcript.md")
             .read_text(encoding="utf-8")),
            ("工具序列正确", info["tools"] == ["Bash", "Write"]),
        ]
        ok = True
        for name, passed in checks:
            print(f"[{'PASS' if passed else 'FAIL'}] {name}")
            ok = ok and passed
        print(f"[{'PASS' if ok else 'FAIL'}] self-test 总体")
        return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="Harbor 跑分产物导出器（可读轨迹 + 原生会话）")
    ap.add_argument("--runs-dir", help="跑分产物根目录，形如 jobs/<job-id>")
    ap.add_argument("-o", "--out", help="导出目录（默认 <runs-dir>/export）")
    ap.add_argument("--to-workx-projects", action="store_true",
                    help="同时装进 ~/.workx/projects/<前缀>-<题名>/ 供本地会话工具查看")
    ap.add_argument("--project-prefix", default="terminal-bench",
                    help="装进 projects 时的目录名前缀（默认 terminal-bench）")
    ap.add_argument("--config-dir", default="", help="workx 配置目录（默认 ~/.workx）")
    ap.add_argument("--taskset-root", default=str(DEFAULT_TASKSET_ROOT),
                    help="Harbor 数据集缓存根（用于取任务原文）")
    ap.add_argument("--model", default="", help="写进会话 session_start 的模型名")
    ap.add_argument("--cwd", default="/app", help="写进会话 session_start 的工作目录")
    ap.add_argument("--copy-raw", action="store_true", help="顺带复制原始 jsonl / 日志")
    ap.add_argument("--self-test", action="store_true", help="用内置样例自检")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    if not args.runs_dir:
        ap.error("需要 --runs-dir，或用 --self-test 自检")

    runs_dir = Path(args.runs_dir)
    streams = find_streams(runs_dir)
    if not streams:
        print(f"未找到任何 artifacts/tmp/workx-stream.jsonl：{runs_dir}", file=sys.stderr)
        return 1

    outdir = Path(args.out) if args.out else runs_dir / "export"
    taskset = Path(args.taskset_root)
    config_dir = Path(args.config_dir) if args.config_dir else Path.home() / ".workx"

    print(f"导出 {len(streams)} 个 trial → {outdir}")
    for sp in streams:
        info = export_one(sp, outdir, taskset, args.model, args.cwd, args.copy_raw)
        print(f"  {info['task']}: {info['steps']} 步 / session {info['session_rows']} 行 "
              f"/ reward {info['reward'] or 'n/a'}")
        if args.to_workx_projects:
            dst = install_to_projects(Path(info["session"]), info["task"], info["session_id"],
                                      config_dir, args.project_prefix)
            print(f"    → 已装入 {dst}")
    print("完成。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
