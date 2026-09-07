# Workx

<div align="center">

<img src="src/icon.png" alt="Workx Icon" width="128" height="128"/>

**Workx** 是一个用纯 C++20 编写的高性能、轻量级终端 AI 编码与任务编排助手，具备普通用户可用的终端 TUI 界面以及可复用的 C++ `workx::agent` 库。

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20NixOS-2ea44f.svg)](#构建)
[![License](https://img.shields.io/badge/license-MIT-purple.svg)](LICENSE)
[![CI](https://github.com/ygsheep/WorkX/actions/workflows/code-quality.yml/badge.svg)](https://github.com/ygsheep/WorkX/actions/workflows/code-quality.yml)

[特性](#-特性总览why-workx) • [终端工具](#-作为终端工具使用for-end-users) • [Harness 库](#-作为-agent-harness-库嵌入for-c-developers) • [构建](#构建) • [测试](#测试)

</div>

***

## 📌 特性

| 卖点                 | 说明                                                                                                                    |
| ------------------ | --------------------------------------------------------------------------------------------------------------------- |
| ⚡ **快与轻量**         | 纯 C++ 原生编译，无需 Node.js / Python 运行时，极速启动，零外部进程开销                                                                       |
| 🤖 **自主 ReAct 推理** | 内置全套工具链（`FileRead` / `FileWrite` / `FileEdit` / `Grep` / `Bash` / `WebFetch` / `AgentTool`），支持子 Agent 并行调度，自主解决复杂工程任务 |
| 🔌 **双模态复用**       | 核心架构与 TUI 彻底解耦，可直接作为第三方 C++ 库链接引入                                                                                     |
| 🇨🇳 **国产模型首等公民**  | 原生预设 DeepSeek、智谱、Kimi、Qwen 等，支持 DeepSeek 提示词缓存实时状态监控                                                                  |

***

## 💻 作为终端工具使用（For End Users）

### 快速开始

**Windows**：前往 [Releases](https://github.com/ygsheep/WorkX/releases) 下载 `workx.exe`，双击运行即可。

**Linux / NixOS**：参考 [Nix 安装指南](docs/nix-guide.md)。

首次运行会启动 Setup Wizard，引导配置 API Key 和选择默认模型。主界面如下：

![启动界面](docs/img/workx_launch.png)

![对话界面](docs/img/workx_chat.png)

### 内置命令

| 命令                | 说明               |
| ----------------- | ---------------- |
| `/help`           | 显示所有可用命令         |
| `/exit` / `/quit` | 退出程序             |
| `/clear`          | 删除当前会话文件并新建会话    |
| `/new`            | 新建会话并切换（保留旧会话文件） |
| `/regen`          | 重新生成上一条回复        |
| `/model`          | 切换当前使用的模型        |

***

## 🛠️ 作为 Agent Harness 库嵌入

`src/core` + `src/agent` 编译为 `workx_core` + `workx_agent` 两个静态库，通过 `find_package(workx)` 或 `add_subdirectory` 引入，无需任何 TUI 依赖。

最小使用只需两行：`Client::create` 配置后端 → `client.chat()` 发送消息。以下示例直接调用真实的 DeepSeek API：

```cpp
#include <iostream>
#include "agent/api/client.h"

using namespace agent;

int main() {
    auto r = Client::create({
        .provider = "deepseek",
        .backend  = { .api_key = "sk-你的DeepSeek-APIKey" }
    });
    if (r.is_err()) {
        std::cerr << "init failed: " << r.error().message << "\n";
        return 1;
    }

    Client client = std::move(r.value());
    auto reply = client.chat("用一句话介绍你自己");
    if (reply.is_ok()) {
        std::cout << reply.value() << "\n";
    }
    return 0;
}
```

> 如需自定义事件驱动循环（订阅 `StreamDoneEvent`、`ToolInvokeBeginEvent` 等），可直接使用 `ChatSession` + `ICompletionProvider` 的底层 API，参考 [`tests/consumer/main.cpp`](tests/consumer/main.cpp)。

> 完整架构设计与工具链路说明见 [技术细节指南](docs/full-guide.md)。

***

## 构建

**前置条件**：CMake 3.21+、C++20 编译器（MSVC 14.5+ / GCC 10+）、[vcpkg](https://github.com/microsoft/vcpkg)

```bash
# Linux
${VCPKG_ROOT}/vcpkg install nlohmann-json catch2 curl tree-sitter
cmake -B build -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake
cmake --build build

# Windows (MSVC)
vcpkg install nlohmann-json catch2 curl
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg_root]/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

## 测试

```bash
cmake --build build --config Release --target <模块>_unit_tests -j 8
ctest --test-dir build -C Release -j 8 --output-on-failure
```

900+ 测试用例，按模块拆分为 `core` / `agent` / `tui` / `island` / `app` 五个独立目标。

## 协议支持

Workx 底层实现两种 LLM 协议适配器，所有模型均通过这两种协议接入，无需为每个厂商单独适配：

| 协议                     | 对应适配器               | 说明                                                                   |
| ---------------------- | ------------------- | -------------------------------------------------------------------- |
| **OpenAI Compatible**  | `openai_adapter`    | 主流开放协议，支持任何兼容端点。DeepSeek、智谱、Kimi、通义千问、MiniMax 等国产模型及 OpenAI 系列均使用此协议 |
| **Anthropic Messages** | `anthropic_adapter` | Anthropic 官方协议，支持 Claude 系列                                          |

内置预设（推荐使用 **DeepSeek**，1M 上下文 + 提示词缓存优化）：

| 提供商             | 预设名称                | 默认模型              |
| --------------- | ------------------- | ----------------- |
| **DeepSeek**    | `deepseek`          | deepseek-v4-flash |
| 智谱 GLM          | `glm`               | glm-5.2           |
| Kimi (Moonshot) | `kimi`              | kimi-k3           |
| 通义千问 (Qwen)     | `qwen`              | qwen-plus         |
| MiniMax         | `minimax`           | MiniMax-M3        |
| 自定义端点           | `openai-compatible` | 自行指定 URL 与模型      |

## 相关文档

- [完整架构与技术细节](docs/full-guide.md)

- [Nix / NixOS 安装](docs/nix-guide.md)

- [Tree-sitter 语法高亮](docs/tree-sitter-guide.md)

- [TUI 渲染管线](docs/tui-render-pipeline.md)

## 许可证

[MIT License](LICENSE)
