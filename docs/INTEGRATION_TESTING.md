# 集成测试指南

> Q-5 文档化：LM Studio LLM 推理测试的手动启动流程，以及自动 Python 测试服务器的使用方式。

## 概述

Workx 集成测试位于 `tests/integration/`，需要外部 HTTP 服务器提供 LLM 推理接口。
提供两种服务器后端，通过环境变量切换：

| 模式 | 服务器 | 启动方式 | 适用场景 |
|------|--------|----------|----------|
| **自动模式（默认）** | Python mock 服务器 | RAII 自动启停 | CI / 日常开发 |
| **手动模式** | LM Studio | 用户预先启动 | 真实 LLM 推理验证 |

## 快速开始

### 1. 自动模式（推荐）

无需任何手动步骤，直接运行：

```bash
# 配置 CMake（启用集成测试）
cmake -B build -DWORKX_BUILD_INTEGRATION_TESTS=ON

# 编译
cmake --build build --config Release

# 运行集成测试（自动启停 Python 服务器）
.\build\bin\Release\workx_integration_tests.exe
```

工作原理：
- `test::AutoTestServer`（[test_server_fixture.h](../tests/integration/test_server_fixture.h)）在测试启动时拉起 `tests/integration/fixtures/test_server.py`
- Python 服务器启动后打印 `TEST_SERVER_PORT=<port>` 到 stdout
- C++ 侧解析端口构造 `http://127.0.0.1:<port>` 作为 base_url
- 测试结束时 RAII 自动终止子进程

Python 服务器模拟以下端点：
- `GET /v1/models` — 返回模型列表
- `POST /v1/chat/completions` — 流式 SSE 响应
- `POST /v1/chat/completions?delay=10` — 慢响应（测试超时）
- `POST /v1/chat/completions?status=500` — 错误响应（测试重试）

### 2. 手动模式（LM Studio）

LM Studio 提供真实 LLM 推理能力，用于验证端到端业务流程。

#### 步骤 1：启动 LM Studio

1. 打开 LM Studio 应用
2. 加载模型（推荐 `Qwen2.5-7B-Instruct` 或 `Llama-3.2-3B-Instruct`）
3. 切换到 "Local Server" 标签
4. 点击 "Start Server"
5. 默认监听 `http://127.0.0.1:1234`

#### 步骤 2：设置环境变量

```bash
# Windows PowerShell
$env:LM_STUDIO_BASE_URL = "http://127.0.0.1:1234"

# Linux/macOS
export LM_STUDIO_BASE_URL=http://127.0.0.1:1234
```

#### 步骤 3：运行集成测试

```bash
.\build\bin\Release\workx_integration_tests.exe
```

`test::AutoTestServer` 检测到 `LM_STUDIO_BASE_URL` 后跳过 Python 服务器，
直接使用 LM Studio 作为测试后端。

### 3. 跳过集成测试

默认 `WORKX_BUILD_INTEGRATION_TESTS=OFF`，集成测试不参与编译：

```bash
cmake -B build  # 不带 -DWORKX_BUILD_INTEGRATION_TESTS=ON
cmake --build build --config Release
# 只有 workx_unit_tests.exe，没有 workx_integration_tests.exe
```

## 测试用例说明

集成测试位于：

| 文件 | 覆盖范围 |
|------|----------|
| [test_http_client.cpp](../tests/integration/test_http_client.cpp) | HttpClient GET/POST、流式 SSE、超时、错误处理、cancel |
| [test_client.cpp](../tests/integration/test_client.cpp) | Client 高层 API、重试逻辑、事件发布 |

测试用例分类：
- **绿色路径**：正常 GET / 流式响应 / 多 chunk 拼接
- **错误路径**：4xx/5xx 响应、网络断开、超时
- **重试逻辑**：H-3 HttpRetryPolicy 的可重试错误触发自动重试
- **生命周期**：cancel 中断、析构清理

## CI 集成

CI 环境使用自动模式（Python 服务器），无需 LM Studio：

```yaml
# GitHub Actions 示例
- name: Configure
  run: cmake -B build -DWORKX_BUILD_INTEGRATION_TESTS=ON

- name: Build
  run: cmake --build build --config Release

- name: Integration Tests
  run: .\build\bin\Release\workx_integration_tests.exe --reporter=compact
```

依赖：
- Python 3.8+（必须可在 PATH 中通过 `python` 或 `python3` 调用）
- Linux 环境使用 `python3`，Windows 使用 `python`

## 故障排查

### Python 服务器启动失败

```
FAILURE: 测试服务器未就绪: http://127.0.0.1:0
```

排查步骤：
1. 确认 Python 已安装：`python --version` 或 `python3 --version`
2. 手动启动测试脚本验证：
   ```bash
   python tests/integration/fixtures/test_server.py
   # 应输出 TEST_SERVER_PORT=<port>
   ```
3. 检查端口占用：`netstat -ano | findstr <port>`

### LM Studio 连接失败

```
FAILURE: 测试服务器未就绪: http://127.0.0.1:1234
```

排查步骤：
1. 确认 LM Studio 已启动 Local Server
2. 浏览器访问 `http://127.0.0.1:1234/v1/models` 应返回 JSON
3. 确认环境变量 `LM_STUDIO_BASE_URL` 设置正确（无末尾斜杠）

### 超时测试失败

慢响应测试（`/v1/chat/completions?delay=10`）依赖 Python 服务器的 `time.sleep`，
若 CI 环境性能不足可能导致 H-2 总时长超时（120s）误触发。
可通过 `--timeout 180` 给 Catch2 更长超时时间。

## 相关文档

- [TECH_DEBT_REGISTRY.md](../plan/TECH_DEBT_REGISTRY.md) — Q-5 验收标准
- [ENVIRONMENT_VARIABLES.md](ENVIRONMENT_VARIABLES.md) — 环境变量完整列表
