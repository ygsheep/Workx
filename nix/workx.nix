# workx.nix — Workx TUI chat client 的 Nix 打包
#
# home-manager 使用（workx.nix / workx.patch 放到配置仓库 nix/ 下）：
#
#   { pkgs, ... }: {
#     home.packages = [
#       (pkgs.callPackage ./nix/workx.nix {
#         src = pkgs.fetchFromGitHub {
#           owner = "young";
#           repo = "Workx";
#           rev = "v0.2.0";
#           hash = "sha256-...";   # 占位 hash,构建报错后填真实值
#         };
#       })
#     ];
#   }
#
# 或 NixOS 系统级安装（fetchFromGitHub 部分同上）：
#
#   { pkgs, ... }: {
#     environment.systemPackages = [ (pkgs.callPackage ./nix/workx.nix { src = ...; }) ];
#   }
#
# 说明：
#   - Nix 构建沙箱无网络，tree-sitter runtime/grammars 的 FetchContent 拉取
#     会失败，因此设 WORKX_FETCH_GRAMMARS=OFF，语法高亮降级为 no-op
#     （渲染管线不受影响，见 src/tui/render/syntax_highlighter.h）。
#   - CMake 的 install 规则只安装 workx_core/workx_agent 库，可执行文件
#     在 postInstall 中手动拷贝（含 icon.png / tools/rg 打包目录）。
#   - ripgrep 通过 wrapProgram 注入 PATH：workx 的 ToolRegistry 按
#     bundled(<exe_dir>/tools/rg) > PATH 顺序解析，Nix 下走 PATH 分支。
#   - 参数 src 由宿主管控：callPackage 时显式传入 workx 源码目录（或
#     builtins.path 过滤后的源码副本）。

{ lib
, stdenv
, cmake
, pkg-config
, python3
, nlohmann_json
, curl
, ripgrep
, makeWrapper
, src
}:

# 对 nixpkgs 的 nlohmann_json 打上 vcpkg 同款补丁：官方 3.12.0 的
# <optional> 在 JSON_HAS_CPP_17 定义前 include，且 optional from_json 被
# JSON_USE_IMPLICIT_CONVERSIONS guard 包住，GCC 14/15 下 value("key",
# std::optional<int>) 的 SFINAE 实例化失败（build patch 见 workx.patch）。
let
  nlohmannJsonPatched = nlohmann_json.overrideAttrs (old: {
    patches = (old.patches or []) ++ [ ./workx.patch ];
  });
in
stdenv.mkDerivation (finalAttrs: {
  pname = "workx";
  version = "0.10.1";

  inherit src;

  nativeBuildInputs = [ cmake pkg-config python3 makeWrapper ];
  buildInputs = [ nlohmannJsonPatched curl ];

  # nixpkgs 的 curl 不安装 CURLConfig.cmake（CONFIG 模式找不到），
  # 改用 CMake 自带 FindCURL 模块（同样提供 CURL::libcurl target）。
  # 只改构建副本，不影响仓库源码。
  patchPhase = ''
    sed -i 's/find_package(CURL CONFIG REQUIRED)/find_package(CURL REQUIRED)/' CMakeLists.txt
  '';

  cmakeFlags = [
    "-DWORKX_FETCH_GRAMMARS=OFF" # 沙箱无网络，禁用 FetchContent
    "-DWORKX_BUILD_TESTS=OFF"
    "-DWORKX_BUILD_EXAMPLES=OFF"
  ];

  # 默认 make install 安装库与公共头；这里追加可执行文件与运行时资源
  # （构建目录为 build/，产物在 build/bin/ 下）
  postInstall = ''
    mkdir -p $out/bin
    cp -r $PWD/bin/. $out/bin/
    # 注入 ripgrep：bundled 缺失时从 PATH 查找
    wrapProgram $out/bin/workx \
      --prefix PATH : ${lib.makeBinPath [ ripgrep ]}
  '';

  meta = {
    description = "Terminal AI chat client with multi-provider support and event-driven architecture";
    homepage = "https://github.com/ygsheep/WorkX";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "workx";
  };
})
