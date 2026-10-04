#!/usr/bin/env bash
# 在 Docker 里重建 Linux 二进制（本机 WSL 被安全策略拉黑，只能在容器里编）
#
# 配方对齐 CI 的 build-test.yml（ubuntu + vcpkg + Ninja Release），
# 区别：这里只编 workx 主程序，不编 4 个测试目标。
#
# ⚠️ 踩过的三个坑（都写死在本脚本里了，别改回去）：
#   1. 镜像必须 >= ubuntu:24.04 —— liblogger/logger.h 用了 `#include <format>`，
#      22.04 的 GCC 11 / libstdc++ 11 没有这个头，configure 能过但编译必挂。
#   2. **构建目录必须落在容器本地**（/build_local），不能挂 Windows bind mount ——
#      MSYS 挂载会让 CMake 探测不到编译器，报 "no CMAKE_CXX_COMPILER could be found"。
#      只有 vcpkg 目录适合挂出来复用（省掉每次重编 curl/openssl 的时间）。
#   3. Git Bash 跑本脚本要加 MSYS_NO_PATHCONV=1（否则 D:/... 会被转成 PortableGit 路径）。
#
# 用法：MSYS_NO_PATHCONV=1 bash scripts/harness/build-linux-container.sh
# 产物：build/linux/workx-linux-amd64（旧版会被备份成 .old-<日期>）
set -euo pipefail

CT=workx-build-linux
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC_HOST="D:/develop/Workspace/workx"
[ -d "$REPO_ROOT/.git" ] && SRC_HOST="$(cd "$REPO_ROOT" && pwd -W 2>/dev/null || echo "$SRC_HOST")"
VCPKG_CACHE="$REPO_ROOT/build/linux/.docker-cache/vcpkg"
mkdir -p "$VCPKG_CACHE"

docker rm -f "$CT" >/dev/null 2>&1 || true
docker run -d --name "$CT" \
  -v "$SRC_HOST:/src" \
  -v "$(cd "$(dirname "$VCPKG_CACHE")" && pwd -W)/vcpkg:/opt/vcpkg" \
  -w / \
  ubuntu:24.04 sleep infinity >/dev/null
echo "[0] 容器已启动 (ubuntu:24.04)"

docker exec "$CT" bash -c '
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build git curl ca-certificates \
  pkg-config unzip zip tar gzip python3 python3-venv \
  autoconf automake libtool bison flex file >/dev/null
echo "[1] apt 依赖装完: $(g++ --version | head -1)"
'

docker exec "$CT" bash -c '
set -e
if [ ! -d /opt/vcpkg/.git ]; then
  git clone --filter=blob:none https://github.com/microsoft/vcpkg.git /opt/vcpkg
fi
if [ ! -x /opt/vcpkg/vcpkg ]; then
  /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics >/dev/null 2>&1
fi
echo "[2] vcpkg ready: $(/opt/vcpkg/vcpkg version | head -1)"
'

# 构建目录必须是容器本地路径（见头部注释坑 #2）
docker exec "$CT" bash -c '
set -e
cmake -S /src -B /build_local -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DWORKX_BUILD_TESTS=OFF \
  -DWORKX_BUILD_EXAMPLES=OFF \
  -DWORKX_WITH_TREE_SITTER=OFF 2>&1 | tail -8
echo "[3] configure 完成"
'

docker exec "$CT" bash -c '
set -e
cmake --build /build_local --target workx -j "$(nproc)" 2>&1 | tail -25
echo "[4] 构建完成"
find /build_local -maxdepth 4 -name workx -type f -perm -u+x | head -3
'

OUT=build/linux/workx-linux-amd64
# ⚠️ docker cp 的目标路径必须是**相对路径**：Windows 盘符 `D:` 里的冒号会和
#    `docker cp <container>:<path>` 的冒号混淆，被解析成第二个 "容器源"，报
#    `invalid output path: directory "D:\d\develop\..." does not exist`。
#    相对路径不含冒号，且同样由 docker 按宿主 CWD 解析（已实测）。
cd "$REPO_ROOT"
mkdir -p "$(dirname "$OUT")"
if [ -f "$OUT" ]; then
  cp "$OUT" "$OUT.old-$(date +%Y%m%d)"
  echo "[5] 旧二进制已备份 -> $(basename "$OUT").old-$(date +%Y%m%d)"
fi
MSYS_NO_PATHCONV=1 docker cp "$CT:/build_local/bin/workx" "$OUT"
echo "[6] 产物已导出 -> $OUT（size=$(wc -c < "$OUT") bytes）"

# ⚠️ 别跳过这一步：曾经拿着没编进改动的旧二进制跑了一整轮对照实验，
#    得出「修复有效」的假结论。签名必须对得上源头。
# --version 输出的 `-g<hash>` 应当等于当前 HEAD 的短 hash。
python scripts/harness/check_binary_signature.py "$OUT"
