# 环境变量参考

> Workx 支持的全部环境变量。配置加载优先级：配置文件 < 环境变量 < CLI 参数。
>
> 自 v2.0.0 起，标准环境变量通过 `ConfigSchema.env_var` 声明，由 `ConfigManager::load_from_env()` 统一加载并按 Schema 类型自动解析/校验。

## 标准环境变量

| 变量名 | 配置键 | 类型 | 解析规则 | 说明 |
|--------|--------|------|----------|------|
| `WORKX_API_KEY` | `backend.api_key` | String | 原值 | 远程 API 密钥 |
| `WORKX_BASE_URL` | `backend.remote_url` | String | 原值 | 远程 API 基础 URL（OpenAI 兼容） |
| `WORKX_MODEL` | `backend.model_name` | String | 原值 | 远程模型名 |
| `WORKX_TIMEOUT` | `backend.timeout_ms` | Int | `std::stoi`，范围 [1, 86400000] | HTTP 超时（毫秒） |
| `WORKX_LOG_LEVEL` | `logging.level` | Enum | 必须为 `trace`/`debug`/`info`/`warn`/`error`/`fatal` | 日志级别 |
| `WORKX_LOG_FILE` | `logging.file` | String | 原值 | 日志文件路径（空禁用文件日志） |

## 特殊语义环境变量

| 变量名 | 配置键 | 语义 | 说明 |
|--------|--------|------|------|
| `WORKX_NO_COLOR` | `terminal.no_color` | presence-only | 环境变量存在且非空即启用，不解析值。兼容 [no-color.org](https://no-color.org) 规范。 |

## 路径环境变量

以下变量不写入配置管理器，由 `app_config.cpp::get_config_dir()` 直接解析，用于定位配置文件与日志文件位置：

| 变量名 | 作用 | 平台 | 优先级 |
|--------|------|------|--------|
| `WORKX_CONFIG_DIR` | 显式指定配置目录 | 全平台 | 最高 |
| `APPDATA` | 应用数据目录（拼 `workx` 子目录） | Windows | 次高 |
| `USERPROFILE` | 用户主目录（拼 `.workx` 子目录） | Windows | 中 |
| `XDG_CONFIG_HOME` | XDG 配置目录（拼 `workx` 子目录） | POSIX | 次高 |
| `HOME` | 用户主目录（拼 `.config/workx` 子目录） | POSIX | 中 |

**回退顺序**：`WORKX_CONFIG_DIR` → 平台变量 → 当前工作目录下的 `.workx`

**最终路径**：
- 配置文件：`<config_dir>/config.json`
- 日志文件：`<config_dir>/logs/workx.log`

## 加载流程

```text
1. register_config_defaults()    # 注册 Schema（含 env_var 绑定）
2. load_from_config_file(path)   # 加载 JSON 配置文件（最低优先级）
3. load_from_env()               # 加载环境变量（中优先级）
   ├─ ConfigManager::load_from_env()  # 按 Schema 加载 6 个标准变量
   └─ 手动处理 WORKX_NO_COLOR         # presence-only 语义
4. CLI 参数解析                   # 最高优先级
```

## 实现要点

- **类型安全**：环境变量值在写入配置前按 `ConfigSchema.type` 解析（Int 用 `std::stoi`，Double 用 `std::stod`，Bool 用 `"true"/"1"` 判定，String/Enum 原值）。
- **解析失败容错**：环境变量值无法按类型解析时静默跳过，不影响其他变量加载。
- **范围/枚举校验**：解析后值仍需通过 Schema 范围（`int_range`/`double_range`）和枚举（`enum_values`）校验，否则拒绝写入并返回错误。
- **空值处理**：环境变量存在但值为空字符串时，String/Enum 类型会写入空字符串；Bool 解析为 `false`；Int/Double 解析抛异常被捕获后跳过。

## 配置键完整列表

详见 [app_config.h](../src/app/config/app_config.h) 中的 `namespace keys`，共 20 个配置项，按模块分组：

- **Terminal**：`simple_io`、`no_color`、`verbose`、`prompt`
- **Backend**：`remote_url`、`model_name`、`api_key`、`provider`、`timeout_ms`、`context_length`
- **Retry**：`retry_count`、`retry_delay_ms`
- **Session**：`system_prompt`、`save_path`
- **Logging**：`level`、`file`
- **Tool — FileRead**：`max_file_size_bytes`、`max_lines_to_read`
- **Tool — FileEdit**：`deny_patterns`、`scan_secrets`

每个配置项的 Schema（类型/默认值/范围/枚举/环境变量映射）在 [app_config.cpp](../src/app/config/app_config.cpp) 的 `register_config_defaults()` 中集中注册。
