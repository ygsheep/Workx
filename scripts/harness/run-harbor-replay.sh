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
#
# 前置：先把 DeepSeek API Key 写进仓库根的 .env.harbor（不要提交）
# 跑前的四道卡口不要跳过，每一道都对应一次真实的翻车：
#   ①二进制签名 ②模型连通 ③数据集行尾（LF）④config 模型名对齐
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
# ⚠️ 模型改名过一次：首轮用的是 `deepseek-v4-flash`，2026-10-04 实测
# /v1/models 只列得出 deepseek-flash / deepseek-v4-pro，但 `deepseek-v4-flash`
# 与 `deepseek-v4-flash-vision-exp` 仍能调用（服务端统一映射到 deepseek-flash）。
# **改模型务必先 curl 打一次 chat/completions 确认 200**，否则整轮跑分全废。
export WORKX_MODEL=${WORKX_MODEL:-deepseek-v4-flash-vision-exp}
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

python - "$SRC_CONFIG" "$OUT_CONFIG" "$MODE" <<'PY'
import json, os, sys
# --- 卡口 ④：job 元信息里的模型名要与容器里实际调用的保持一致 ---
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
json.dump(c, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
print(f"✅ 已生成 {out}：{len(c['datasets'][0]['task_names'])} 题")
PY

echo "=== 开始跑分 (mode=$MODE) ==="
harbor run -c "$OUT_CONFIG" --env-file .env.harbor -y
