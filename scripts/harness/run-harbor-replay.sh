#!/usr/bin/env bash
# 复跑首轮那 20 题（Issue #142 / #133 / #137 验证）
#
# 设计要点：**只让二进制这一个变量变**。
# 通过 `--config` 直接喂 JobConfig JSON，把 datasets / agents / mounts / 并发数 /
# artifacts 与首轮 JobConfig 对齐 —— 唯一区别是新编译的 workx-linux-amd64。
# 这是首轮以来第一次能称得上「对照实验」的配置。
#
# 用法：
#   MSYS_NO_PATHCONV=1 bash scripts/harness/run-harbor-replay.sh          # 20 题
#   MSYS_NO_PATHCONV=1 bash scripts/harness/run-harbor-replay.sh smoke    # 3 题
#   MSYS_NO_PATHCONV=1 bash scripts/harness/run-harbor-replay.sh dry      # 只跑卡口 + 生成配置，不调 harbor（不花钱）
#
# 前置：先把 DeepSeek API Key 写进仓库根的 .env.harbor（不要提交）
#       跑分前先起代理桥：bash scripts/harness/harbor-proxy.sh start
# 跑前的六道卡口不要跳过，每一道都对应一次真实的翻车：
#   ①二进制签名 ②模型连通 ③数据集行尾（LF）④config 模型名对齐 ⑤代理桥连通
#   ⑥每题墙钟预算可推导（#166 方案 C）
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

export MSYS_NO_PATHCONV=1          # Git Bash 会把 D:/... 转成 PortableGit 下的路径
export PATH="$HOME/.local/bin:$PATH"   # harbor 由 uv tool 装在这里，非交互 shell 不带此路径
# ⚠️ 必须显式转成 Windows 路径：上面开了 MSYS_NO_PATHCONV=1（docker 挂载需要），
# 它会顺带**关掉** Git Bash 对 PYTHONPATH 的路径转换 —— 于是 Windows 版 Python
# 收到 /d/develop/... 这种 POSIX 路径，sys.path 里就找不到 agents.workx。
export PYTHONPATH=$(pwd -W)
export WORKX_BASE_URL=https://api.deepseek.com
# ⚠️ 模型改名过两次：首轮是 `deepseek-v4-flash`；2026-10-04 实测
# `deepseek-v4-flash` / `deepseek-v4-flash-vision-exp` 仍能调用（服务端统一映射到 deepseek-flash）；
# 2026-10-11 起官方名称即 `deepseek-flash`（见 .env.harbor.example）。
# **改模型务必先 curl 打一次 chat/completions 确认 200**，否则整轮跑分全废。
export WORKX_MODEL=${WORKX_MODEL:-deepseek-flash}
# ⚠️ 必须抬高：http_client.cpp:394 把总超时取 max(timeout_ms, 120000)，
# 默认值 30000 会被拉到 **120 秒**。thinking 模型在长任务中后段（上下文 35KB+）
# 单次生成很容易超过 2 分钟 → `Total request timeout exceeded` →
# react_loop.cpp:869 直接 break，**整轮 Thought 失败即终止且无重试**，
# trial 就这样废了（adaptive-rejection-sampler 实测死于 iteration=5）。
export WORKX_TIMEOUT=${WORKX_TIMEOUT:-600000}
export WORKX_AGENT_BINARY_PATH=/opt/workx-bin/workx-linux-amd64

MODE=${1:-full}
SRC_CONFIG=jobs/2026-10-03__13-45-16/config.json
OUT_CONFIG=build/linux/replay20.json

if [ ! -f .env.harbor ]; then
  echo "❌ 缺少 .env.harbor，请先创建并写入：WORKX_API_KEY=<你的 DeepSeek key>" >&2
  exit 1
fi

# --- 卡口 ①：产物二进制真的含待验证改动 ---
# 曾拿没编进改动的旧二进制跑完一整轮，得出「修复有效」的假结论。
python scripts/harness/check_binary_signature.py build/linux/workx-linux-amd64

# --- 卡口 ②：模型真的能调通，别等一小时才发现整轮空转 ---
python - <<'PY'
import json, os, urllib.request

key = ""
for line in open(".env.harbor", encoding="utf-8"):
    if line.startswith("WORKX_API_KEY="):
        key = line.split("=", 1)[1].strip()
model = os.environ.get("WORKX_MODEL", "")
base = os.environ.get("WORKX_BASE_URL", "https://api.deepseek.com")
req = urllib.request.Request(
    base.rstrip("/") + "/chat/completions",
    data=json.dumps(
        {"model": model, "messages": [{"role": "user", "content": "say OK"}], "max_tokens": 16}
    ).encode(),
    headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"},
)
with urllib.request.urlopen(req, timeout=60) as r:
    body = json.loads(r.read())
print(f"✅ 模型连通：{model} -> HTTP {r.status} / 服务端 {body.get('model')}")
PY

# --- 卡口 ③：数据集行尾必须 LF ---
# CRLF 会让 checksum 校验题必挂、容器里 *.sh 报 bad interpreter，
# 表现为「verifier 没跑完」这种看起来像 agent 失败的噪声。
python scripts/harness/fix_dataset_lineendings.py --check \
  || { echo "请跑：python scripts/harness/fix_dataset_lineendings.py --fix"; exit 1; }

# --- 卡口 ⑤：代理桥连通（#166 方案 A）---
# 容器直连境外数据集/源码会被干扰（caffe 实测 SSL UNEXPECTED_EOF / aria2c 报错 /
# wget 92KB/s），挂代理后 apt/git 秒过。桥不通就别开跑，否则整轮都耗在重试上。
# 桥的启动与 docker 构建期代理见 scripts/harness/harbor-proxy.sh。
# 必须 export：下面的 python 块是独立进程，普通 shell 变量读不到
export PROXY_ENV_JSON="$(bash scripts/harness/harbor-proxy.sh env-json)"
bash scripts/harness/harbor-proxy.sh check

python - "$SRC_CONFIG" "$OUT_CONFIG" "$MODE" <<'PY'
import json, os, sys
# --- 卡口 ④：job 元信息里的模型名要与容器里实际调用的保持一致 ---
# --- 运行期注入：environment.env 写入代理（#166 方案 A）---
src, out, mode = sys.argv[1:4]
c = json.load(open(src, encoding="utf-8"))
if mode == "smoke":
    # 选首轮因 uv 下载失败而没有任何有效判定的题 —— 最直接的「修复是否生效」观测点
    watch = {"custom-memory-heap-crash", "adaptive-rejection-sampler", "caffe-cifar-10"}
    c["datasets"][0]["task_names"] = [t for t in c["datasets"][0]["task_names"] if t in watch]
c.pop("job_name", None)          # 让 harbor 用时间戳建新 job 目录，别覆盖旧产物
WANT_MODEL = os.environ.get("WORKX_MODEL", "")
for a in c.get("agents", []):
    if a.get("model_name") and a["model_name"] != WANT_MODEL:
        print(f"⚠️  config 里 model_name {a['model_name']} -> {WANT_MODEL}（与 WORKX_MODEL 对齐）")
        a["model_name"] = WANT_MODEL

# 容器内代理地址是 host.docker.internal（宿主桥），不是 127.0.0.1；
# no_proxy 已排除 *.deepseek.com —— LLM 调用绕行代理会拖慢且代理断了整轮废。
proxy_env = json.loads(os.environ.get("PROXY_ENV_JSON", "") or "{}")
if proxy_env:
    env_section = c.setdefault("environment", {}).setdefault("env", {})
    env_section.update(proxy_env)
    print(f"✅ 已注入运行期代理 env：{proxy_env['http_proxy']}（no_proxy 含 .deepseek.com）")
else:
    print("⚠️  未拿到代理 env（PROXY_ENV_JSON 为空），容器将直连外网")

json.dump(c, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
print(f"✅ 已生成 {out}：{len(c['datasets'][0]['task_names'])} 题")
PY

# --- 卡口 ⑥：每题墙钟预算可推导（#166 方案 C）---
# 适配器（agents/workx）按题读 task.toml 的 [agent] timeout_sec 推导
# WORKX_AGENT_TIMEOUT_SEC（×0.9）。有题读不到 → 那题不下发墙钟 → agent 不知道 deadline，
# 回到 #137 的原始问题（轮次还够、墙钟先到 → 被硬超时杀 → AgentTimeoutError）。
# 故逐题校验，读不到即整体失败。
python - "$OUT_CONFIG" "$MODE" <<'PY'
import json, os, re, sys

out, mode = sys.argv[1:3]
cfg = json.load(open(out, encoding="utf-8"))
root = cfg["datasets"][0]["path"]
names = cfg["datasets"][0]["task_names"]
MARGIN = 0.9


def agent_timeout(task_dir):
    try:
        text = open(os.path.join(task_dir, "task.toml"), encoding="utf-8").read()
    except OSError:
        return None
    try:
        import tomllib

        value = (tomllib.loads(text).get("agent") or {}).get("timeout_sec")
        return float(value) if value else None
    except Exception:
        pass
    # 老解释器兜底：分节感知，别把 [verifier]/[environment] 的 timeout 误当 agent 的
    section = ""
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("[") and s.endswith("]"):
            section = s.strip("[]").strip()
            continue
        if section == "agent":
            m = re.match(r"timeout_sec\s*=\s*([0-9]+(?:\.[0-9]+)?)", s)
            if m:
                return float(m.group(1))
    return None


missing, budgets = [], []
for name in names:
    t = agent_timeout(os.path.join(root, name))
    if not t:
        missing.append(name)
    else:
        budgets.append((name, t, int(t * MARGIN)))

if missing:
    print(f"❌ 卡口⑥失败：{len(missing)} 题读不到 [agent] timeout_sec → 这些题不会下发墙钟：")
    for m in missing[:8]:
        print(f"    - {m}")
    sys.exit(1)

lo = min(b for _, _, b in budgets)
hi = max(b for _, _, b in budgets)
print(
    f"✅ 卡口⑥ 墙钟预算可推导：{len(budgets)} 题，预算 {lo}~{hi}s"
    f"（题面 timeout_sec × {MARGIN}，由适配器按题下发）"
)
if mode == "dry":
    for name, t, b in budgets:
        print(f"    {name:<32} timeout_sec={t:>7.0f}s -> WORKX_AGENT_TIMEOUT_SEC={b}s")
PY

# dry 模式：卡口全跑、配置照常生成，但不实际调用 harbor（不花钱）。
# 用途：改完 harness 脚本先验证「签名/模型/数据集/代理/注入」这条链路是通的。
if [ "$MODE" = "dry" ]; then
  echo "=== dry 模式：跳过 harbor 调用 ==="
  python - "$OUT_CONFIG" <<'PY'
import json, sys
c = json.load(open(sys.argv[1], encoding="utf-8"))
env = c.get("environment", {}).get("env", {})
print(f"题目数       : {len(c['datasets'][0]['task_names'])}")
print(f"agent        : {c['agents'][0]['name']} / model={c['agents'][0].get('model_name')}")
print(f"代理 env 注入: {json.dumps(env, ensure_ascii=False, indent=2) if env else '（无）'}")
PY
  exit 0
fi

echo "=== 开始跑分 (mode=$MODE) ==="
harbor run -c "$OUT_CONFIG" --env-file .env.harbor -y
