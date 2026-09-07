# Nix / NixOS 安装指南

> Nix 构建完全抛弃 vcpkg / FetchContent 体系：`nix/` 目录下的 `workx.nix` + `workx.patch` 把依赖全部换成 nixpkgs 提供。
>
> 其中 `workx.patch` 对 nlohmann_json 3.12.0 打上 vcpkg 同款补丁——官方版 nlohmann_json 在 GCC 下调用 `value("key", std::optional<T>)` 会因 SFINAE 顺序问题实例化失败，补丁修复了模板重载顺序。

---

## 方式一：Flake（推荐，NixOS 系统安装）

### 直接嵌入到 NixOS `configuration.nix`（flake 模式）

在 NixOS 系统的 `flake.nix` 中加入 input：

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    workx = {
      url = "github:ygsheep/WorkX/develop";   # 指定 develop 分支为源码
      inputs.nixpkgs.follows = "nixpkgs";     # 对齐系统 nixpkgs（避免两次构建）
    };
  };

  outputs = { self, nixpkgs, workx, ... }@inputs: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ({ pkgs, ... }: {
          environment.systemPackages = [
            inputs.workx.packages.${pkgs.system}.default
          ];
        })
      ];
    };
  };
}
```

### home-manager 同理

```nix
home.packages = [ inputs.workx.packages.${pkgs.system}.default ];
```

### 仓库内直接试用（临时 / 本地构建）

```bash
# 仅构建，结果到 ./result
nix build .#default

# 或永久安装到用户 profile
nix profile install .#default
```

---

## 方式二：callPackage（home-manager / NixOS 直接引用源码包）

不使用 flake，把本仓库当作普通 deriviation 用 `pkgs.callPackage` 消费。

**步骤**：将 `nix/workx.nix` 与 `nix/workx.patch` 拷入你的配置仓库（两个文件必须保持同目录，patch 路径通过 `./workx.patch` 相对引用），`src` 指向 Workx 源码：

```nix
{ pkgs, ... }:
{
  home.packages = [
    (pkgs.callPackage ./nix/workx.nix {
      src = pkgs.fetchFromGitHub {
        owner = "ygsheep";                  # 仓库实际 owner/repo
        repo = "Workx";
        rev = "develop";                    # 分支或 tag
        hash = "sha256-...";                # 先用占位，构建报错后 Nix 会提示真实 hash
      };
    })
  ];
}
```

---

## Nix 特有说明

| 项目 | 行为 | 原因 / 说明 |
|------|------|------------|
| **Tree-sitter grammar 拉取** | 强制设 `WORKX_FETCH_GRAMMARS=OFF`，语法高亮降级为 no-op（渲染管线不受影响，代码块只缺配色不缺内容） | Nix 构建沙箱是纯离线（`__networkPhase` 之后无网络），FetchContent 从 GitHub clone 30 个 grammar 仓库必然失败 |
| **CMake install 产物** | install 规则只安装 `workx_core` / `workx_agent` 两个静态库；可执行文件 `workx`、`icon.png`、`tools/rg`（ripgrep）通过 `postInstall` 手动 `cp` 到 `$out/bin`/`$out/share` | CMakeLists.txt 的 install(TARGETS) 显式列出的 TARGET 只有库，可执行文件被故意排除（因为它是参考宿主，不是 Harness 库的一部分） |
| **ripgrep 查找顺序** | 通过 `wrapProgram --prefix PATH : ${pkgs.ripgrep}/bin` 注入 PATH，Nix 下走 PATH 分支 | workx 解析 ripgrep 路径按 `bundled(<exe_dir>/tools/rg)` > PATH 顺序；Nix 没法把 ripgrep 拷进 `<exe_dir>/tools/`（store 只读），所以走 PATH |
| **测试** | `doCheck = true` 默认启用，跑全部 5 个 module 的 unit_tests（core/agent/tui/island/app）；`[slow]` 标签的超时/并发类测试通过 CTEST_TEST_FILTER 自动排除（Nix 沙箱有时限） | 900+ 用例无网络（mock provider + mock event bus），全部能离线跑 |
