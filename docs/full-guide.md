# Workx 完整扩展文档（Full Guide）

> 🔙 **回到入口**：[README.md](../README.md) 是项目极简入口（两动线快速上手）。
>
> 本文档是 **README 的完整扩展版**，包含：架构设计、PlanMode V2、七步工具漏斗、四级 Token 压缩、四层失败兜底、Harness 设计说明、Linux/Windows 构建、命令参考、测试分层等全部技术细节。

---

<div align="center">

<img src="../src/icon.png" alt="Workx Icon" width="128" height="128"/>

**基于 ReAct 循环与工具调用架构的现代终端 Code Agent / Work Agent，能够自主完成编码、调试、文件操作与任务编排**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20NixOS-2ea44f.svg)](#构建步骤)
[![License](https://img.shields.io/badge/license-MIT-purple.svg)](../LICENSE)
[![CI](https://github.com/ygsheep/WorkX/actions/workflows/code-quality.yml/badge.svg)](https://github.com/ygsheep/WorkX/actions/workflows/code-quality.yml)

[功能特性](#特性) • [快速开始](#构建步骤) • [运行](#运行) • [命令参考](#命令参考) • [配置](#配置) • [测试](#测试)

</div>

---

> 一个现代化的终端 Code Agent / Work Agent，基于 ReAct 循环与工具调用架构，能够自主完成编码、调试、文件操作与任务编排。
>
> **Agent Harness 定位**：除终端客户端外，`src/core` + `src/agent` 构成可复用的 Agent Harness 库（`workx::agent`）。外部工程可通过 `find_package(workx)` 或 `add_subdirectory` 链接并驱动 ReAct Agent 循环（注入 `ICompletionProvider`、订阅 `EventBus` 事件即可），**无需任何 TUI/应用层依赖**——`src/tui` 与 `src/app` 只是参考宿主实现。消费示例见 `tests/consumer/`。

## 特性

- **自主任务执行**: 基于 ReAct 循环（推理 + 行动），Agent 自主规划并调用工具完成任务，而非简单问答
- **工具调用能力**: 内置文件读写、编辑、Glob/Grep 搜索、Bash 执行、Web 抓取、子 Agent 调度等工具
- **多提供商支持**: 内置 OpenAI、Anthropic、DeepSeek 等预设，一键切换
- **权限控制系统**: 对敏感工具调用进行权限校验，支持多种权限模式
- **上下文管理**: 自动 Token 压缩与记忆管理，长任务也能保持连贯上下文
- **MCP 协议集成**: 通过 Model Context Protocol 接入外部数据源与工具
- **SSE 流式响应**: 实时显示 Agent 推理与工具调用过程
- **Markdown 渲染**: 支持表格、标题、列表、代码块（Tree-sitter 语法高亮，支持 30 种语言）、强调等完整 Markdown 语法
- **Diff 可视化**: 文件编辑以带背景色的 Diff 形式呈现，清晰展示增删改动
- **命令系统**: `/help`、`/exit`、`/clear`、`/regen`、`/model` 等内置命令
- **自定义模型**: 支持添加自定义模型和 API 端点
- **彩色输出**: 终端彩色渲染，语法高亮的代码块

## 架构概览

![Workx 四层架构图（鲸鱼娘解说版）](architecture_overview.jpg)

一个现代化的终端 Code Agent / Work Agent，基于 ReAct 循环与工具调用架构，能够自主完成编码、调试、文件操作与任务编排。

**Agent Harness 定位**：除终端客户端外，`src/core` + `src/agent` 构成可复用的 Agent Harness 库（`workx::agent`）。外部工程可通过 `find_package(workx)` 或 `add_subdirectory` 链接并驱动 ReAct Agent 循环（注入 `ICompletionProvider`、订阅 `EventBus` 事件即可），**无需任何 TUI/应用层依赖**——`src/tui` 与 `src/app` 只是参考宿主实现。消费示例见 `tests/consumer/`。

### 模块依赖关系

![模块依赖关系图（Mermaid 渲染）](img/11_module_dependency.png)

依赖方向为**单向分层**（无循环依赖）：`src/tui` 与 `src/island` 依赖 `src/agent` + `src/core`，`src/agent` 依赖 `src/core`，`src/core` 无内部模块依赖。源文件见 [`module_dependency.mmd`](module_dependency.mmd)。

### 核心工作流（鲸鱼娘图解）

| 📐 ReAct 推理循环 | 📡 EventBus 跨层中枢 |
|---|---|
| ![ReAct 推理循环](img/01_react_loop.jpg) | ![EventBus 跨层事件中枢](img/02_eventbus_flow.jpg) |
| Thought → Action → Observe 循环 ≤25 轮，无工具调用即输出 FinalAnswer | 发布-订阅解耦四层：TUI/Agent/Core 异步消息驱动，同步 publish 需防死锁 |

### Plan Mode V2 五阶段编排（鲸鱼娘图解）

![PlanMode V2 五阶段状态机（Interview→Exploring→Planning→AwaitingApproval→Done）](img/characters/13_plan_mode_v2_whale.jpg)

当用户需要对大型变更"先规划再动手"时，ChatSession 会在 ReAct 循环外额外启动一层语义级编排器 `PlanCoordinator`，按 **五阶段状态机** 流转（对应 `src/agent/plan/session_plan_state.h` 的 `PlanStage` 枚举）：

| 阶段 | 触发 | 做什么 | 产出 |
|------|------|--------|------|
| **1 Interview** | 用户发起 `/plan` 或 EnterPlanModeTool | 通过 BriefTool 向用户澄清需求、收集约束、列出模糊点并追问 | `interview_notes` 结构化需求清单 |
| **2 Exploring** | Interview 结束或用户跳过 | 并行启动 N 个只读 Plan 子 Agent（`permission_mode=Plan` 双防线：schema 白名单过滤 + AgentTool 子 registry 再过滤），每个 scan 一个子域 | `ExploreFinding[]` 每子域 summary + critical_files |
| **3 Planning** | 所有 explore 子 Agent 完成 | plan Agent 综合 findings，输出结构化实施计划（步骤、风险、关键文件、跨子域去重 critical_files） | `PlanArtifact{summary, steps, risks, critical_files[], findings, markdown}` |
| **4 AwaitingApproval** | Planning 产出写入 `~/.workx/plan/plan_<session>.md` | 侧边栏渲染 Markdown 方案 + 风险清单，展示 critical_files 跳转链接；用户可批准/驳回/修改 | 用户决策事件 |
| **5 Done** | 用户批准 | ExitPlanModeV2Tool 解析 `critical_files[]` 写入 `ExitPlanModeEvent::critical_files`，ChatSession 把产物合并进消息前缀，回到普通 ReAct 循环按计划执行 | 进入执行阶段 |

> **关键设计**：explore task_id 采用 `pa-<cycle>-<n>` 格式（单调递增 plan_cycle），确保跨规划轮次全局唯一；rejected 时回退到 Interview 重新修订。关键文件清单（critical_files）在 ChatSession 侧被程序消费——执行阶段自动把它们置顶到 FileReadState 预读取缓存。

```
src/
├── agent/              # Agent 核心层
│   ├── api/            # LLM 后端接口（OpenAI/Anthropic 适配器、SSE 流、HTTP 客户端）
│   ├── command/        # 命令执行器与注册表
│   ├── compact/        # Token 压缩与上下文截断
│   ├── core/           # 会话管理、查询引擎、ReAct 推理循环
│   ├── input/          # 输入解析与处理
│   ├── message/        # 消息构建与历史管理
│   ├── model/          # 模型配置、提供商预设、模型路由
│   ├── permission/     # 权限校验、权限模式、规则定义
│   ├── prompt/         # 系统提示词与记忆管理
│   ├── tool/           # 工具集（Agent 自主调用的能力）
│   │   ├── AgentTool/      # 子 Agent 调度（任务分解与并行执行）
│   │   ├── BashTool/       # Shell 命令执行
│   │   ├── FileEditTool/   # 文件精确编辑
│   │   ├── FileReadTool/   # 文件读取
│   │   ├── FileWriteTool/  # 文件写入（含 Diff 生成）
│   │   ├── GlobTool/       # 文件名模式匹配
│   │   ├── GrepTool/       # 内容正则搜索
│   │   ├── MCPTool/        # Model Context Protocol 工具桥接
│   │   └── WebFetchTool/   # 网页内容抓取
│   └── util/           # 异步、JSON Schema、字符串工具
├── app/                # 应用层
│   ├── command/        # 内置系统命令
│   ├── config/         # 配置管理、CLI 参数解析
│   ├── ui/             # 模型选择器、路径补全、文件索引
│   └── main.cpp        # 程序入口
├── core/               # 核心基础设施
│   ├── config/         # 配置管理器
│   ├── events/         # 事件总线（驱动 Agent 与 UI 解耦）
│   ├── task/           # 任务管理器
│   └── utils/          # Result 类型等通用工具
└── tui/                # 终端用户界面
    ├── core/           # 终端封装、屏幕管理、显示缓冲（Win32/POSIX）
    ├── input/          # 行编辑器、输入历史
    ├── render/         # 聊天渲染、Markdown 渲染、Tree-sitter 语法高亮、流式缓冲
    ├── setup/          # 设置向导
    ├── utils/          # UTF-8 工具函数
    └── widgets/        # 状态栏、命令面板、文件搜索面板、底部栏
```

## 支持的 AI 提供商

仅内置中国顶级模型厂商预设，另支持自定义 OpenAI 兼容端点：

| 提供商 | 预设名称 | 默认模型 |
|--------|----------|----------|
| DeepSeek | `deepseek` | deepseek-v4-flash |
| 智谱 GLM | `glm` | glm-5.2 |
| Kimi (Moonshot) | `kimi` | kimi-k3 |
| 通义千问 (Qwen) | `qwen` | qwen-plus |
| MiniMax | `minimax` | MiniMax-M3 |
| Custom URL | `openai-compatible` | (自定义) |

![LLM 后端适配层（插件式 8 家 Provider）](img/08_backend_adapter.jpg)

> 架构分层：上层 `ReActLoop/ChatSession` 只依赖 `ICompletionProvider + IBackendAdmin` 双接口，新增 Provider 只需实现一个 Adapter。

## Agent 工具能力

Workx 的核心是 Agent 自主调用工具完成任务。内置工具集如下：

| 工具 | 说明 |
|------|------|
| `FileReadTool` | 读取文件内容，支持行范围与偏移 |
| `FileWriteTool` | 创建或覆盖文件，并生成 Diff |
| `FileEditTool` | 基于字符串替换的精确编辑 |
| `GlobTool` | 按文件名 glob 模式快速查找文件 |
| `GrepTool` | 基于 ripgrep 的内容正则搜索 |
| `BashTool` | 执行 Shell 命令（受权限系统管控） |
| `WebFetchTool` | 抓取并提取网页内容 |
| `AgentTool` | 启动子 Agent 处理子任务，支持并行调度 |
| `MCPTool` | 桥接 MCP 服务器，接入外部工具与数据源 |

![工具调用全管线（权限 + 密钥脱敏 + 并行执行）](img/04_tool_pipeline.jpg)

> 鲸鱼娘提醒版：
> ![工具管线提醒](img/characters/04_tool_pipeline_whale.jpg)

每一次 LLM 请求的 tool_calls 在真正执行前会经过 `ToolExecutor::execute()` 的 **七步漏斗**（严格按序，任一步失败立即中断，全部留审计日志 `AuditLogger::log_tool_invoke`）：

| 步骤 | 模块 | 做什么 | 失败行为 |
|------|------|--------|----------|
| **① lookup_tool** | `ToolRegistry` | 按名称查工具实例；不存在记一条 `deny: tool not found` 审计 | `Error::ResourceNotFound` |
| **② 模式守卫 + 取消信号** | `SessionMode` / `ctx.is_cancelled()` | `Minimal` 模式白名单只放行 `Skill/Bash/Read/Write/Edit`（LLM 幻觉出其他工具名第二道拦截）；执行前检测 `cancel_flag` | `PermissionDenied` / `Cancelled` |
| **③ PermissionRequest Hook** | `HookManager::dispatch(PermissionRequest)` | #50 通用 Hook 体系：在正式权限检查前派发，hook 脚本可按工具名+输入动态授权/阻断（匹配不到零开销） | `PermissionDenied`（带 hook 返回的错误原因） |
| **④ check_permissions** | `ITool::check_permissions` + `ask_user_confirm` | 四层权限模式：`ManualApproval`（弹 AskUserPanel Yes/No 模态）→ `BypassPermissions`（全放行）→ `Plan`（只读模式阻断所有写/执行工具，与子 Agent 白名单过滤形成双防线纵深防御）→ 危险命令命中 `shell_guard`（见下文）**fail-closed**：`event_bus_ptr` 空、超时、用户点 No、模态被取消 → 全部 false 拒绝 | `PermissionDenied` |
| **⑤ validate_input** | `ITool::validate_input` + `json_schema` | 按工具 `input_schema` JSON Schema 校验字段类型、必填项、枚举范围 | `InvalidInput` |
| **⑥ run_with_safety** | `try/catch` 五层包装 + `shell_guard` + `secret_scanner` | 内部实际 `tool.call()`，异常全捕获：`json::exception` / `filesystem_error` / `bad_alloc`（OOM）/ `std::exception` / 未知异常 → 统一映射 `Error::Code`。Bash/Write 工具内部额外调用：**shell_guard**（3类风险：破坏性命令 rm -rf~/format C:/shutdown 等 / SSRF 内网IP 169.254/10/127/172.16-31/192.168 / env 泄露）、**secret_scanner**（13条密钥规则：AWS/GCP/GitHub/GitLab/Slack/Anthropic/OpenAI/Stripe/PrivateKey，写入前阻断 + bash 输出时 `[REDACTED:X]` 脱敏）+ CWD allowlist 限制 | `ToolExecutionFailed` / `InternalError` |
| **⑦ finalize_result** | UTF-8 清洗 + `truncate_result()` + 审计 | ① `sanitize_utf8` 去除非法字节（避免 GBK 子进程污染 JSON 序列化）② 结果 > 8KB 时保留 **头 4KB + 尾 4KB**（中间省略含截断字节数，UTF-8 边界安全截断）③ `was_truncated` 标记给下游 UI 提示 | 成功返回 `ExecutionResult` |

> **shell_guard 三类风险**（来自 `agent/tool/shell_guard.cpp`）：
> - **Destructive 破坏性命令**：token 化判定 `rm -rf` 后跟 `/` `~` `*` `..` 等根目标、`mkfs`/`dd if=/dev/`/`of=/dev/sd`、`format C:`、`del /f/s/q C:`、`shutdown/reboot`、`reg delete HKLM`
> - **SSRF 云元数据/内网**：命令中裸 URL（http://）指向 `169.254.169.254`（云元数据）、`127/8`、`10/8`、`172.16/12`、`192.168/16`、`100.64/10` CGNAT、`localhost`
> - **EnvLeak 环境变量泄露**：独立 token `env` / `printenv`（排除 `env KEY=VAL cmd` 设置形式）、`/proc/*/environ`、PowerShell `gci env:`
>
> **secret_scanner 工作双模式**（来自 `agent/tool/secret_scanner.cpp`，规则移植自 Claude Code secretScanner.ts）：
> - **写入阻断模式**：`FileWriteTool`/`FileEditTool` 调用前 `scan_for_secret_error()`，命中任何密钥规则 → 返回错误不写入
> - **输出脱敏模式**：`BashTool` 输出和 tool_result 回写消息时 `redact_secrets()`，匹配到的密钥替换为 `[REDACTED:规则标签]`（日志审计仍保留原始明文，脱敏只面向 LLM 防止回显泄露）

**MCP 跨进程工具桥接**（JSON-RPC stdio/HTTP 双传输）：

![MCP 跨进程工具桥接](img/09_mcp_bridge.jpg)
![MCP 桥梁角色说明](img/characters/09_mcp_bridge_whale.jpg)

通过 `MCPClientManager` 按配置启动/连接外部 MCP Server，MCPTool 的 prompt() 里动态列出 server 名与工具列表，call() 时按 tool_name 路由到对应 server 的 `tools/call`。`AgentTool` 支持子作用域 MCP（`mcpServers` 字段）——每个子 Agent 独立 MCP 连接池，父级不污染子级。

## 上下文管理 & Token 压缩

![Token 上下文压缩 + 预算面板](img/05_token_compression.jpg)
![鲸鱼娘解说版](img/characters/05_token_compression_whale.jpg)

**预算面板（实时展示）**：
- StatusBar 显示 4 类计数（Prompt / Cache Create / Cache Read / Generated）+ 估算费用
- 侧边栏展示会话累计 Token、DS 缓存命中率（`DS 缓存 命中 80% (8k/10k)`）与 Prompt/生成分项（#65）

---

### 四级水位压缩策略（CacheAwareCompactor）

`src/agent/compact/cache_aware_compactor.cpp` 按 **token / 上下文窗口** 的 ratio，从轻到重走四个水位，对应 `Config` 里的 `soft_ratio/snip_ratio/compact_ratio/force_ratio`（默认 0.60 / 0.75 / 0.85 / 0.95），核心思路是**只动中段，钉住前缀和尾部**，最大化 DeepSeek / OpenAI Prompt Cache 命中率。

#### **永远不折叠的两个区域**

```
 messages: [prefix pinned] ─── [middle foldable] ─── [tail preserved]
          ← pinned_prefix_len →               ← tail_start →
```

| 区域 | 长度计算 | 说明 |
|------|---------|------|
| **前缀钉住区** | `pinned_prefix_len()`：首条 user 消息（≤ min(1500, 窗口×15%) 时钉住）+ 历史 `<compaction-summary>` 摘要消息 | 保证 Prompt Cache 的 prefix 字节级稳定，下一轮请求 cache_read 命中；首条 user 过大时不钉（避免预算浪费） |
| **尾部保留区** | `tail_start()`：从尾部往前累积 token 到 `tail_token_budget`（默认 16384），对齐到非 tool 消息（避免 orphan tool_result 造成 API 报错） | 保留最近完整的多轮对话 + 工具调用对，模型能看到近因 |

#### **水位① Soft Notice (ratio < 0.75)**

不做任何修改，仅日志 `cache soft notice: tokens=X ratio=0.6X`，给上游 UI 预算面板回调变色提示。

#### **水位② Snip (0.75 ≤ ratio < 0.85)**

中段区域（pinned 之后、tail 之前）内的 **tool_result 按策略机械截短**（不调用 LLM，纯本地 O(n)，不破坏 prefix 形状，不递增 `rewrite_version`）：

| 工具类型 | SnipStrategy | 效果 |
|---------|-------------|------|
| 只读类工具（Read/Glob/Grep/WebFetch） | 保留 `[snipped: head=20% ... middle omitted N chars ... tail=20%]` | 大文件/长 grep 输出的关键头尾还在 |
| 副作用类工具（Bash/Write/Edit） | 保留前 3 行 + 最后 3 行 | 构建命令、编译器警告/错误在尾部 |
| 已含 `[snipped` 标记的消息 | 跳过 | 避免重复截短 |

#### **水位③ Compact (0.85 ≤ ratio < 0.95)**

中段所有消息（整条 middle，含 user/assistant/tool）**折叠为 1 条 `<compaction-summary>…</compaction-summary>` user 消息**，放入 pinned 之后：

1. **调用注入的 `SummarizeFn`（LLM 摘要）**；未注入时 fallback 到 `mechanical_fold_summary`（机械拼接每条消息角色+前 200 字预览）
2. **DS_CACHE M-1 归档**：若配置了 `archive_dir`，middle 原消息按 NDJSON（每行一条 `{role,content,tool_name,…}`）写入 `<archive_dir>/<YYYYMMDD_HHMMSS>_<ms>.jsonl`，并在摘要末尾追加 `<!-- archive: path (N messages) -->` 注解，完整可追溯
3. 原地替换：`messages.erase(pinned_end…tail_start)` → `insert(summary_msg)`，**递增 `rewrite_version`**（破坏了 prefix 形状，Prompt Cache 需要重建）

#### **水位④ Force (ratio ≥ 0.95)**

行为同 Compact，action 标记 `Force`，日志里区分，用于 UI/metrics 告警统计。

#### **卡死守卫（H-3 自愈）**

为防止"compact 一次没降多少 → 下一轮又超 → 再 compact → 死循环每分钟重写 middle 导致 Prompt Cache 反复失效"：连续 compact 次数 `consecutive_compacts ≥ max_consecutive_compacts`（默认 3）时设 `m_stuck=true`，自动压缩暂停 → 后续请求纯 append-only（前缀稳定）→ token 掉回 soft（0.6）以下自动清除 `m_stuck` 恢复压缩。回调 `paused_cb(true/false, consecutive, tokens, ratio, notice)` 供 UI 提示。

---

### 回退兜底链（压缩也救不回时）

1. **ContextCompressor 精简版保底**（早期实现，ChatSession 在 CacheAwareCompactor 不可用或配置关闭时调用）：max_messages 硬上限（超出裁最旧消息）→ 旧 tool_result 压缩成 100 字 `[Result of Read: 内容…]`
2. 总 Token 仍超窗口上限：ReActLoop 暂停多工具并行（一次只调用一个，减少 tool_calls payload）
3. 最终极限：抛 `ContextOverflowError`，ChatSession 发布终止事件，UI 提示"请新建会话或清理历史"

## 失败兜底机制

![四层兜底盾牌（HTTP重试→异常捕获→Fail-Closed→卡死守卫）](img/characters/11_failure_fallback_whale.jpg)

Workx 对每一层可能失败的点都做了独立闭环，从网络到工具到压缩到权限共 **四层兜底盾牌**：

| 层 | 失败场景 | 兜底策略 | 源码位置 |
|----|---------|---------|---------|
| **① 网络/HTTP 层** | LLM 请求 429 限流、500/502/503/504 服务器错、断连（curl 返回 0 状态码无响应体）、超时断开 | **HttpRetryPolicy 指数退避**：`delay(attempt) = min(base_delay_ms * 2^attempt, max_delay_ms)`，默认 `max_retries=3`（1s/2s/4s，上限 60s，≥63 attempt 防 1<<63 UB 直接 cap）。业务错误 `max iterations`（ReAct 跑完了）/ 4xx 客户端错 **不重试**。与 Error::is_retryable() 位码对齐，StreamSession 总时长超时配合 H-2 总超时切断 | `src/agent/api/retry.h` `HttpRetryPolicy::is_retryable()` + `delay()` |
| **② 工具执行层** | Bash 脚本失败、JSON 解析异常、文件系统权限错误、OOM bad_alloc、未知异常抛到栈顶 | **run_with_safety 五层 try/catch + BashTool 四层守卫**：`nlohmann::json::exception` → ToolExecutionFailed；`filesystem_error` → ToolExecutionFailed；`bad_alloc` → InternalError；`std::exception` → ToolExecutionFailed；`catch(...)` → Unknown。Bash 另外加 Sandbox 适配 + 超时 kill（后台任务存入 `BashOutputRegistry` 50 条上限 FIFO，避免内存膨胀） | `src/agent/tool/executor.h` `run_with_safety()` + `bash_tool.cpp` `BashOutputRegistry` |
| **③ 权限确认层** | 用户 30s 不点击 Yes/No、宿主程序无 TUI（`event_bus_ptr == nullptr`）、AskUser 面板被取消 | **fail-closed 三分支全部拒绝**：`event_bus_ptr == nullptr` → false 直接返回；`future.wait_for(timeout_ms) != ready` 超时 → false；`!result.submitted` 面板取消 → false。没有任何一条路径默认放行。Plan 只读模式双防线（check_permissions 拒绝写/执行 + AgentTool 子 registry 构建时再过滤非只读工具） | `src/agent/tool/permission_ask.cpp` `ask_user_confirm()` + `agent_tool.cpp` 子 registry 构建时过滤 |
| **④ 压缩卡死层** | 连续 3+ 次 compact 仍降不到 soft（典型：用户追问每次都追加 8K 字，LLM 摘要精度不够降不下来） | **卡死守卫暂停自动压缩**：`consecutive_compacts ≥ 3` 时 `m_stuck=true`，接下来的请求走 append-only，让 prefix 重新稳定（Prompt Cache 命中），token ratio 自然掉到 soft 以下自动清除 stuck。回调 `paused_cb` 发事件给 StatusBar 提示"压缩暂停"。ContextOverflowError 前最后一道防线 | `src/agent/compact/cache_aware_compactor.cpp` `maybe_compact()` H-3 分支 + `CacheAwareCompactor::Result::Action::Stuck` |

## 前置条件

![项目依赖全景图（核心库 + 可选库分层说明）](img/10_dependency_overview.jpg)

- **CMake**: 3.21 或更高版本
- **C++ 编译器**: 支持 C++20（MSVC 14.5+ 或 GCC 10+）
- **vcpkg**: 依赖包管理器，依赖库如下：

| 库 | 说明 |
|----|------|
| `nlohmann-json` | JSON 解析（所有平台） |
| `catch2` | 单元测试框架（所有平台） |
| `curl` | HTTP 客户端（所有平台） |
| `tree-sitter` | 语法高亮（所有平台） |
| `imgui` | 调试用原生窗口（仅 Windows，`dx11-binding` + `win32-binding`） |

## 构建步骤

> **注意**: Windows 为完整支持平台；Linux（含 NixOS）亦受支持，NixOS 安装见 [nix-guide.md](nix-guide.md)。

![CMake 跨平台构建管线（vcpkg + FetchContent 一键三平台）](img/07_build_pipeline.jpg)
![构建管线角色说明](img/characters/07_build_pipeline_whale.jpg)

### Linux

```bash
# 初始化 vcpkg 依赖（首次使用，${VCPKG_ROOT} 为你的 vcpkg 安装路径）
${VCPKG_ROOT}/vcpkg install nlohmann-json catch2 curl tree-sitter

# 配置 CMake
cmake -B build -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build build
```

### Windows (MSVC)

```bash
# 初始化 vcpkg（首次使用）
vcpkg install nlohmann-json catch2 curl

# 创建构建目录
mkdir build
cd build

# 配置 CMake（替换 [vcpkg_root] 为你的 vcpkg 安装路径）
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg_root]/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build . --config Release
```

## 语法高亮（Tree-sitter）

基于 Tree-sitter 运行时（vcpkg 提供）+ CMake FetchContent 动态拉取 30 种 grammar，当前裁剪到 c/cpp/python/js/go/rust/java 等 30 种主流语言后 workx.exe ≈ 44MB。

- 开关：`WORKX_WITH_TREE_SITTER`（默认 ON），`WORKX_FETCH_GRAMMARS`（Nix 无网络需 OFF）
- 新增/恢复语言：改 `scripts/gen_ts_grammars.py` 的 `GRAMMARS` 后跑脚本自动生成 cmake 注册 + 头文件声明

📖 **完整操作、体积说明、构建坑点见独立文档**：[tree-sitter-guide.md](tree-sitter-guide.md)

## Nix / NixOS 安装

通过 flake 一键嵌入 NixOS 或 home-manager，依赖完全由 nixpkgs 提供（不使用 vcpkg/FetchContent），`workx.nix` 自带 nlohmann_json GCC SFINAE 补丁。

**快速体验**（本仓库内）：
```bash
nix build .#default          # 构建 → ./result
nix profile install .#default # 安装到用户 profile
```

📖 **flake / callPackage 两种集成方式、沙箱限制说明见独立文档**：[nix-guide.md](nix-guide.md)

## 运行

![Setup Wizard 首次启动引导（4 步配置）](img/06_setup_wizard.jpg)
![首次启动向导角色说明](img/characters/06_setup_wizard_whale.jpg)

```bash
# Windows
build\bin\workx.exe

# Linux/macOS
./build/bin/workx
```

首次运行时会启动设置向导，引导您配置 API Key 和选择默认模型。

## 命令参考

| 命令 | 说明 |
|------|------|
| `/help` | 显示所有可用命令 |
| `/exit` / `/quit` | 退出程序 |
| `/clear` | 删除当前会话文件并新建会话 |
| `/new` | 新建会话并切换（保留旧会话文件） |
| `/regen` | 重新生成上一条回复 |
| `/model` | 切换当前使用的模型 |

## TUI 渲染注意事项（开发必读）

Resize → Begin → 分层渲染 → End → Flush 五步严格顺序，违反会出现光标乱飞/快照失效/死锁（已踩坑合集）。

🚨 **五步完整顺序、每步事故记录、修复 Checklist 见独立文档**：[tui-render-pipeline.md](tui-render-pipeline.md)

## 设计说明：Harness 链路定位与为什么需要这些工具

![workx_core + workx_agent = 可嵌入的 Agent Harness 库](img/characters/12_harness_positioning_whale.jpg)

### Harness 是什么：`workx_core` + `workx_agent` 两层库

Workx 的源码按「是否与终端 UI 绑定」严格分层，CMake `install()` 规则只安装两个静态库目标（**不**含 `workx_tui` / `workx_app`），外部工程走 `find_package(workx)` 或 `add_subdirectory` 即可直接驱动 ReAct Agent 循环——**不需要拉 FTXUI、不需要 Win32/POSIX 终端封装、不需要设置向导**。这就是我们说的 "Agent Harness"：

```
┌─────────────────────────────────────────────────────────┐
│                    你的 App (宿主层)                      │
│   TUI 参考实现 (workx_tui/workx_app)  ·  Web  ·  CLI     │
│   只需要：注入 ICompletionProvider → 订阅 EventBus →     │
│            send_message() → drain_async_events()         │
└───────────────┬─────────────────────────────┬─────────────┘
                │  ChatSession 公共 API      │  事件：StreamTokenEvent
                │  send_message() / plan     │       StepDoneEvent
                ▼                             ▼       StreamDoneEvent
┌─────────────────────────────────────────────────────────┐
│                 workx_agent (Harness 上层)              │ ← 纯逻辑
│  ChatSession · ReActLoop · ToolExecutor 7步漏斗         │    无 UI
│  PlanCoordinator 5阶段 · CacheAwareCompactor            │    依赖仅
│  HookManager · McpClientManager · SessionStore          │  workx_core
└───────────────────┬─────────────────────────────────────┘
                    │ Config / Events / Task / Utils / Error
                    ▼
┌─────────────────────────────────────────────────────────┐
│                 workx_core  (Harness 底层)              │ ← 零依赖
│  ConfigManager · EventBus · TaskManager(线程池)          │    纯 C++20
│  ToolRegistry · SubProcess · SandboxAdapter             │    + nlohmann_json
└─────────────────────────────────────────────────────────┘
```

#### **最小宿主（无 TUI 驱动冒烟，完整见 `tests/consumer/main.cpp`）**

```cpp
// ── 1. 装配最小宿主依赖（全部来自 public API）──
EventBus&      bus   = EventBus::instance();
TaskManager&   tm    = TaskManager::instance();
ConfigManager& cfg   = ConfigManager::instance();
auto session = std::make_unique<ChatSession>(
    std::make_unique<YourCompletionProvider>(),  // 自己接 LLM 后端
    tm, bus, cfg, 1000, "my-session-id");

// ── 2. 订阅事件（不订阅也能跑，拿不到输出而已）──
std::atomic<bool> done{false};
std::string      reply;
auto token = bus.subscribe<StreamDoneEvent>([&](const StreamDoneEvent& e){
    reply = e.full_content; done.store(true);
});

// ── 3. 主循环（外部工程自己的 event loop）──
session->send_message("帮我重构一下 main.cpp，删掉 dead code");
while (!done.load()) {
    bus.drain_async_events();  // 自己选 sleep 节拍
    std::this_thread::sleep_for(20ms);
}
bus.unsubscribe<StreamDoneEvent>(token);
// → reply 里就是 Agent 的最终回复（含工具调用结果）
```

外部宿主可以按需额外订阅：`ToolInvokeBeginEvent/ToolInvokeEndEvent`（工具调用对钩 UI 进度条）、`AskUserRequestEvent`（如果宿主支持权限面板）、`CompactionPausedEvent`（压缩卡死死守通知）。**workx_agent 绝不阻塞宿主线程**——所有推理/工具/压缩都在 `TaskManager` 后台线程池里跑，宿主通过 drain 异步事件拉结果。

---

### 为什么需要这些 Tool（7步漏斗每层的设计理由）

很多同行 Code Agent 用"LLM 返回 tool_use → 拿到函数名反射调用"这种极简模式上线，上线一周后几乎都会遇到同一组事故：**LLM 幻觉出 `rm -rf /` 跑了 / API Key 被写进 git / Bash 打印的密钥被模型看到又 echo 回去 / 内网元数据被 curl 到**。Workx 每一层工具链路都是针对这些真实事故设计的：

| 链路环节 | 不做会出什么事故 | 为什么这么设计 |
|---------|----------------|--------------|
| **② Minimal Mode 守卫** | 用户让模型"只做纯思考 + 读写文件"，LLM 幻觉出了 Bash/Grep 把整个系统改了 | 两道防线：Tool 注册表里 `input_schema` 过滤 → Minimal 白名单第二道拦截（schema 漏网的也被拒），给用户"极简安全模式"选项而不是空口承诺 |
| **③ PermissionRequest Hook** | 企业内部想按 AD 组限制谁能跑 Bash / 特定项目只能读 `src/` | HookManager 支持 hook 脚本用 glob 匹配 tool_name+输入，动态授权/阻断；**不用重编**workx_agent，外部宿主扔一个 `hooks/danger_bash.yaml` 就行 |
| **④ check_permissions + ask_user_confirm fail-closed** | Agent 前端卡死后权限线程挂起，默认 `true` 通过了 `rm -rf` | fail-closed 三分支全 false：`event_bus_ptr` 空（Harness 被嵌入到无 UI 的后台服务）→ 直接拒；超时 → 拒；用户取消 → 拒。**没有任何一条路径在"不确定用户意图"时默认通过**。Plan 只读模式双防线（check_permissions 拒绝写/执行 + AgentTool 子 registry 构建时再过滤非只读工具），因为子 Agent 是新 loop，逃逸一次就是灾难 |
| **⑥ shell_guard 三类风险** | LLM 生成了 `curl http://169.254.169.254/latest/meta-data/` 拿云凭据 → 发给攻击者；`rm -rf /*` 跑起来 0.3s 全挂 | token 化判定（避免 `printf("format %d")` 误伤 `format C:`）；IP 网段判断含 IPv4-mapped IPv6（`::ffff:169.254...`）；env 泄露排除 `env KEY=VAL cmd` 正常设置形式。覆盖了 Claude Code、OpenBash 在 issue tracker 里真实出现过的所有破坏性命令变体 |
| **⑥ secret_scanner 写入阻断 + 输出脱敏** | 开发者测试时写了 `sk-ant-api03-xxx` 到源代码 → Agent commit 推到 GitHub → 密钥被扫描器秒刷；Bash 跑 `env` 看到 `OPENAI_KEY=xxx` → LLM 多轮对话里回显给终端用户 | 两种模式分开：写入前**阻断**（不写就不会被 git 带走），输出时**脱敏**（审计日志留明文，只对 LLM 可见内容打码，因为 LLM 会"复述看到的密钥"这是已知 prompt injection 攻击面）。规则直接移植 Claude Code 线上跑了两年的 secretScanner.ts，13 条覆盖主流厂商 + PEM 私钥 |
| **⑦ finalize_result UTF-8 清洗 + 8KB 截断** | Windows 下 `cl.exe` 输出 GBK 字节 → nlohmann JSON 序列化抛 `type_error.316` → 整个 ReAct loop 崩；`grep -r` 扫 node_modules 返回 200MB 文本 → 上下文直接撑爆被模型拒 | UTF-8 清洗只做"把非法字节替换成 替换字符（U+FFFD）"，不做转码（不引入 iconv 依赖）；截断保留头 4KB + 尾 4KB，**UTF-8 边界安全回退**（截断点回退到非 10xxxxxx 字节，避免砍断多字节字符产生新的非法 UTF-8）；`was_truncated` 标记给 UI 显示 `output truncated` 提示 |
| **HttpRetryPolicy 指数退避** | 部署尖峰 LLM 同时 429 + 503 → 客户端立刻重试 → 流量加倍 → 雪崩 → 被封禁 1h | 退避上限 60s（`min(base * 2^attempt, 60s)`），避免 30+ 次重试到小时级；`attempt≥63` 直接 cap 防有符号 int64 `1<<63` UB；业务错误 `max iterations` 明确不重试（那是 ReAct 正常终止，不是网络故障） |
| **CacheAwareCompactor 4级水位** | 长会话到 80% 窗口 → 直接砍老对话 → Prompt Cache 下一轮 0% 命中 → 费用×10 → 用户账单爆炸；到 95% 才压缩 → 一次请求失败就 ContextOverflow | 分 4 级从轻到重：soft(60%) 不动 → snip(75%) 只动中段 tool_result（不破坏 prefix，Prompt Cache 继续命中）→ compact(85%) LLM 摘要（归档 NDJSON 可追溯）→ force(95%) 告警。卡死守卫防止连续 compact 导致的 prefix 抖动（最大的费用杀手不是 token 多，是 Prompt Cache 每轮都 miss） |
| **PlanMode V2 5阶段** | 用户："重构一下 AgentTool 整套" → LLM 不知道有哪些文件 → ReadTool 扫 50 个文件 → 改 1 个文件后才发现依赖 3 个头文件 → 重写 → 发现冲突 → 反复返工 → 用了 2 小时 30 万 token 还没好 | Interview 先问清模糊点（要保留什么？改不改接口？测试要求？）→ Exploring N 个只读子 Agent 并行 scan 每个子域（子 Agent 只带 Read/Glob/Grep 三种工具，绝不会改错文件）→ Planning 统一汇总 findings 产出结构化方案 + risks + critical_files → 批准后再执行。对 3 人日以上的任务，时间/费用都可节省 50%+ |

> 以上设计原则可以概括为一句话：**"对 LLM 的每一次输出，默认它是恶意的/错的/会被注入的，然后一层层收紧"**。Harness 层不做"信任"，只做"验证"。TUI 可以换、LLM Provider 可以换、Skill 和 MCP 可以无限扩展，但 Harness 内的 7 步漏斗 + 4 级压缩 + fail-closed 永远不做"为了方便"的默认放行开关。

## 配置

配置文件位于用户目录下，包含 API Key、默认模型、超时设置等。设置向导会在首次运行时生成配置。

## 测试

单元测试已**按模块拆分为 5 个独立目标**，与 `src/` 分层对齐，解决编译慢与运行慢的问题（改某模块只重编/重链该模块，各模块可并行编译与运行）：

| 目标 | 覆盖目录 | 链接库 |
|---|---|---|
| `core_unit_tests` | `tests/unit/core/**` | `workx_core` |
| `agent_unit_tests` | `tests/unit/agent/**`（含 `helpers` 自测） | `workx_agent` |
| `tui_unit_tests` | `tests/unit/tui/**` | `workx_tui` |
| `island_unit_tests` | `tests/unit/island/**` | `workx_island` |
| `app_unit_tests` | `tests/unit/app/**` | `workx_app` |

```bash
# 构建某个模块测试（<模块> 为 core/agent/tui/island/app）
cmake --build build --config Release --target <模块>_unit_tests -j 8

# 并行运行全部测试
ctest --test-dir build -C Release -j 8 --output-on-failure

# 快速回归：跳过 [slow] 慢测试（验证超时/并发类逻辑被打上 [slow] 标签）
ctest --test-dir build -C Release -LE slow -j 8

# 只跑慢测试
ctest --test-dir build -C Release -L slow -j 8

# 按功能标签或名称过滤
ctest --test-dir build -C Release -L <tag> -j 8
ctest --test-dir build -C Release -R <name> -j 8

# 单测可执行文件按 Catch2 标签过滤
build/bin/Release/core_unit_tests.exe "[skill]"
```

> 源码自动收集：模块测试源文件经 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 收集，新增测试文件无需手改 CMake 列表；`catch_discover_tests(ADD_TAGS_AS_LABELS ON)` 将 Catch2 标签（含 `[slow]`）映射为 ctest 标签，从而支持 `-L / -LE` 过滤。

```bash
# 运行集成测试 (需启用 -DWORKX_BUILD_INTEGRATION_TESTS=ON, 且需 LM Studio)
build/bin/workx_integration_tests.exe
```

项目包含 900+ 个测试用例，覆盖核心功能模块。

## 许可证

MIT License
