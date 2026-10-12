#!/usr/bin/env bash
# 评测环境网络代理桥（#166 方案 A：网络代理固化）
#
# 背景：跑分容器直连境外数据集/源码会被干扰——实测 caffe 题下载 CIFAR-10
# （cs.toronto.edu）出现 SSL `UNEXPECTED_EOF`、aria2c 直接报错退出、wget 单连接
# 仅 ~92 KB/s 且反复断流；而挂上代理后 apt-get / git clone 秒过。
#
# 但本机只有 socks5 代理（127.0.0.1:10808），而 wget/aria2c/apt 对 socks5 支持
# 不全（aria2c 只认 `--all-proxy`，且不读环境变量）。所以本脚本在宿主起一层
# **HTTP → SOCKS5 桥**（pproxy，监听 0.0.0.0:18888），容器经
# `host.docker.internal:18888` 走完整链路：
#
#     容器 --HTTP--> 宿主桥(pproxy) --SOCKS5--> 上游代理 --> 外网
#
# 两个注入点（缺一不可，分别对应「构建期」和「运行期」）：
#   1. 运行期：JobConfig 的 `environment.env` 注入 http_proxy/https_proxy
#      （`env-json` 子命令输出该 dict，由 run-harbor-replay.sh 写进配置）；
#   2. 构建期：docker build / compose build 要拉 apt/pip 包，走的是宿主 docker
#      客户端 —— 在 ~/.docker/config.json 写 `proxies` 段（`start` 子命令负责），
#      docker 会把它转成 build-arg 注入构建容器。
#
# ⚠️ 必须排除 DeepSeek API（no_proxy）：LLM 调用绕行代理会明显变慢，
#    且代理一断整轮跑分全废。
#
# 用法：
#   MSYS_NO_PATHCONV=1 bash scripts/harness/harbor-proxy.sh status     # 看桥与 docker 配置状态
#   MSYS_NO_PATHCONV=1 bash scripts/harness/harbor-proxy.sh start      # 起桥 + 注入 docker 构建代理
#   MSYS_NO_PATHCONV=1 bash scripts/harness/harbor-proxy.sh check      # 只看桥通不通（exit 1 = 不通）
#   MSYS_NO_PATHCONV=1 bash scripts/harness/harbor-proxy.sh env-json   # 打印运行期注入用 JSON
#   MSYS_NO_PATHCONV=1 bash scripts/harness/harbor-proxy.sh docker-off # 撤销 docker 构建代理
#   MSYS_NO_PATHCONV=1 bash scripts/harness/harbor-proxy.sh stop       # docker-off + 停桥
#
# 环境变量（都有默认值）：
#   WORKX_SOCKS5_PROXY    上游 socks5，默认 socks5://127.0.0.1:10808
#   WORKX_PROXY_BRIDGE    桥监听地址，默认 0.0.0.0:18888
#   WORKX_PROXY_NO_PROXY  直连白名单，默认见下方 DEFAULT_NO_PROXY
set -euo pipefail

export MSYS_NO_PATHCONV=1
export PATH="$HOME/.local/bin:$PATH"   # pproxy 由 uv tool 装在这里

SOCKS5="${WORKX_SOCKS5_PROXY:-socks5://127.0.0.1:10808}"
BRIDGE="${WORKX_PROXY_BRIDGE:-0.0.0.0:18888}"
BRIDGE_HOST="${BRIDGE%%:*}"
BRIDGE_PORT="${BRIDGE##*:}"
[ "$BRIDGE_PORT" = "$BRIDGE" ] && BRIDGE_PORT=18888
# 容器视角的宿主地址：Docker Desktop 下固定是 host.docker.internal
CONTAINER_HOST="host.docker.internal"

# 直连白名单：DeepSeek API 绝不能走代理（会拖慢 + 代理一断全轮跑分废）
DEFAULT_NO_PROXY="localhost,127.0.0.1,::1,host.docker.internal,.deepseek.com,api.deepseek.com"
NO_PROXY_LIST="${WORKX_PROXY_NO_PROXY:-$DEFAULT_NO_PROXY}"

DOCKER_CFG_LABEL="~/.docker/config.json"
# ⚠️ 下面所有碰 ~/.docker/config.json 的 python 代码都**不接收路径参数**，
#    而是在 python 侧用 os.path.expanduser("~") 自己算。原因：本脚本开了
#    MSYS_NO_PATHCONV=1（docker 挂载需要），Git Bash 不再把 `/c/Users/...`
#    转成 `C:/Users/...`，Windows 版 python 会把 "/c/Users/..." 当成
#    **当前盘符**下的相对绝对路径（在 D: 盘跑就写进 D:\c\Users\...），
#    静默写错位置、真实配置反而没改。
PROBE_URL="https://raw.githubusercontent.com/BVLC/caffe/master/README.md"

# 探活：经宿主侧 HTTP 桥拉一个稳定的境外小文件。
# 只探宿主侧（127.0.0.1:PORT）——容器侧依赖 docker 网络，由 run-harbor-replay.sh
# 跑分前的实际下载验证，不在本脚本重复。
#
# ⚠️ 别写成 `-o /dev/null`：本脚本开了 MSYS_NO_PATHCONV=1（docker 挂载需要），
#    Git Bash 便不再把 /dev/null 转成 NUL，Windows 版 curl 会报
#    `(23) client returned ERROR on write` —— 表现为「桥明明通、探活说不行」。
#    用 shell 重定向（由 bash 自己处理，不受该变量影响）才稳。
probe_bridge() {
  curl -x "http://127.0.0.1:${BRIDGE_PORT}" -sS --max-time 15 "$PROBE_URL" >/dev/null 2>&1
}

# 找监听端口的进程（Windows Git Bash 用 netstat；失败不致命，仅用于打印 pid）
bridge_pid() {
  netstat -ano 2>/dev/null | grep -E "[.:]${BRIDGE_PORT}[[:space:]]" | grep -i listening | awk '{print $NF}' | head -1
}

cmd_status() {
  local ok="❌ 不通" pid
  probe_bridge && ok="✅ 通畅"
  pid="$(bridge_pid)"
  echo "上游 socks5 : $SOCKS5"
  echo "宿主桥      : $BRIDGE  [$ok]  ${pid:+pid=$pid}"
  echo "容器视角    : http://${CONTAINER_HOST}:${BRIDGE_PORT}"
  echo "no_proxy    : $NO_PROXY_LIST"
  if python - <<'PY' 2>/dev/null
import json, os, sys
p = os.path.join(os.path.expanduser("~"), ".docker", "config.json")
try:
    cfg = json.load(open(p, encoding="utf-8"))
except (OSError, ValueError):
    sys.exit(1)
sys.exit(0 if cfg.get("proxies") else 1)
PY
  then
    echo "docker 构建代理: ✅ 已配置（$DOCKER_CFG_LABEL）"
  else
    echo "docker 构建代理: ⚪ 未配置（构建期拉包不走代理，跑 start 配置）"
  fi
}

cmd_check() {
  if probe_bridge; then
    echo "✅ 代理桥连通：http://127.0.0.1:${BRIDGE_PORT} -> $SOCKS5"
    return 0
  fi
  echo "❌ 代理桥不通（先跑：bash scripts/harness/harbor-proxy.sh start）" >&2
  return 1
}

cmd_env_json() {
  # 运行期注入用：容器内地址是 host.docker.internal，不是 127.0.0.1
  python - "$CONTAINER_HOST" "$BRIDGE_PORT" "$NO_PROXY_LIST" <<'PY'
import json, sys
host, port, no_proxy = sys.argv[1:4]
url = f"http://{host}:{port}"
print(json.dumps({
    "http_proxy": url, "https_proxy": url,
    "HTTP_PROXY": url, "HTTPS_PROXY": url,
    "no_proxy": no_proxy, "NO_PROXY": no_proxy,
}, ensure_ascii=False))
PY
}

cmd_start() {
  if probe_bridge; then
    echo "[1/2] 桥已在运行（复用），pid=$(bridge_pid)"
  else
    command -v pproxy >/dev/null 2>&1 || {
      echo "❌ 找不到 pproxy（uv tool install pproxy 装到 ~/.local/bin）" >&2; exit 1; }
    echo "[1/2] 启动 HTTP→SOCKS5 桥：$BRIDGE -> $SOCKS5"
    # nohup + 后台：pproxy 是常驻服务，日志留在 build/linux/ 便于排查
    mkdir -p build/linux
    nohup pproxy -l "http://${BRIDGE}" -r "$SOCKS5" > build/linux/pproxy.log 2>&1 &
    local waited=0
    until probe_bridge; do
      waited=$((waited + 1))
      if [ "$waited" -ge 20 ]; then
        echo "❌ 桥起不来（等了 10s），看 build/linux/pproxy.log" >&2
        echo "   常见原因：端口 $BRIDGE_PORT 被别的程序占用，或上游 socks5 $SOCKS5 不通" >&2
        exit 1
      fi
      sleep 0.5
    done
    echo "      ✅ 桥已就绪，pid=$(bridge_pid)"
  fi

  echo "[2/2] 注入 docker 构建代理（$DOCKER_CFG_LABEL 的 proxies 段）"
  python - "http://${CONTAINER_HOST}:${BRIDGE_PORT}" "$NO_PROXY_LIST" <<'PY'
import json, os, shutil, sys
url, no_proxy = sys.argv[1:3]
# 路径在 python 侧解析：见文件头 DOCKER_CFG_LABEL 处的 MSYS 说明（不能用参数传）
cfg_path = os.path.join(os.path.expanduser("~"), ".docker", "config.json")
cfg = {}
if os.path.exists(cfg_path):
    with open(cfg_path, encoding="utf-8") as f:
        text = f.read().strip()
    cfg = json.loads(text) if text else {}
    # 备份一次，便于手工回滚
    backup = cfg_path + ".workx-backup"
    if not os.path.exists(backup):
        shutil.copy2(cfg_path, backup)
old = cfg.get("proxies", {}).get("default", {})
cfg.setdefault("proxies", {})["default"] = {
    "httpProxy": url, "httpsProxy": url, "noProxy": no_proxy,
}
os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
with open(cfg_path, "w", encoding="utf-8") as f:
    json.dump(cfg, f, ensure_ascii=False, indent=2)
    f.write("\n")
if old:
    print(f"      （覆盖了原有 proxies.default：{old}）")
print("      ✅ 构建期代理已配置：docker build/compose build 会经代理拉包")
PY
  echo
  echo "下一步：bash scripts/harness/run-harbor-replay.sh（卡口⑤会自动校验桥连通并注入运行期 env）"
}

cmd_docker_off() {
  python - <<'PY'
import json, os, sys
cfg_path = os.path.join(os.path.expanduser("~"), ".docker", "config.json")
if not os.path.exists(cfg_path):
    print("⚪ ~/.docker/config.json 不存在，无需处理")
    sys.exit(0)
with open(cfg_path, encoding="utf-8") as f:
    text = f.read().strip()
cfg = json.loads(text) if text else {}
if cfg.pop("proxies", None) is not None:
    with open(cfg_path, "w", encoding="utf-8") as f:
        json.dump(cfg, f, ensure_ascii=False, indent=2)
        f.write("\n")
    print("✅ 已移除 docker 构建代理（proxies 段）")
else:
    print("⚪ docker 配置里没有 proxies 段，无需处理")
PY
}

cmd_stop() {
  cmd_docker_off
  local pid
  pid="$(bridge_pid)"
  if [ -n "$pid" ]; then
    # Windows 上 taskkill 按 PID 杀；Git Bash 的 kill 对原生进程不一定生效
    taskkill //PID "$pid" //F >/dev/null 2>&1 || kill "$pid" 2>/dev/null || true
    echo "✅ 已停止桥进程 pid=$pid"
  else
    echo "⚪ 端口 $BRIDGE_PORT 上没有监听进程"
  fi
}

case "${1:-status}" in
  status)    cmd_status ;;
  check)     cmd_check ;;
  env-json)  cmd_env_json ;;
  start)     cmd_start ;;
  docker-off) cmd_docker_off ;;
  stop)      cmd_stop ;;
  *)
    echo "用法：$0 {status|check|env-json|start|docker-off|stop}" >&2
    exit 2
    ;;
esac
