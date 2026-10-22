{
  description = "Workx TUI chat client - flake 提供 devShell 与可安装 package";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs =
    { self, nixpkgs }:
    let
      # 版本号单一事实源：cmake/version.cmake 会校验此字面量
      version = "0.10.3";
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = nixpkgs.lib.genAttrs systems;

      # flake 源码本体（self.outPath）排掉 build/.git/.claude/vendor 后再打包，
      # 避免把构建产物和仓库元数据带进 src。
      src = builtins.path {
        path = self.outPath;
        name = "workx-src";
        filter = path: type:
          let base = baseNameOf path;
          in !(type == "directory" && (
            base == "build" || base == ".git" || base == ".claude" || base == "vendor"
          ));
      };
    in
    {
      # 可直接安装的 package：NixOS/home-manager 端用
      #   inputs.workx.packages.${pkgs.system}.default
      # 或 `nix profile install .#default` / `nix build .#default`
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          workx = pkgs.callPackage ./nix/workx.nix { inherit src; };
          default = self.packages.${system}.workx;
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
        in
        {
          default = pkgs.mkShell {
            name = "workx-dev";
            env = {
              # NixOS 上没有 /lib64/ld-linux-x86-64.so.2，跑不了 vcpkg 下载的
              # 通用 Linux 预编译工具（cmake/ninja/git 等）。强制 vcpkg 全部使用
              # Nix 环境自带的二进制，避免 stub-ld: exit 127。
              VCPKG_FORCE_SYSTEM_BINARIES = "1";
              # 供 CMakePresets.json 的 $env{VCPKG_ROOT} 自动定位 vcpkg toolchain。
              VCPKG_ROOT = "${pkgs.vcpkg}/share/vcpkg";
            };
            packages = with pkgs; [
              gcc
              cmake
              ninja
              pkg-config
              vcpkg
              python3
              git
              gnumake
              patch
              gzip
              bzip2
              xz
              unzip
              gnutar
              autoconf
              automake
              libtool
            ];
          };
        });

    };
}
