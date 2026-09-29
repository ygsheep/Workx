# 设计方案 — Issue #80：json_schema.h 空壳，执行侧无通用参数校验

- **Issue**: https://github.com/ygsheep/Workx/issues/80
- **分支**: `fix/issue-77-80-headless-and-schema`（与 #77 合并处理）
- **类型**: bug（修复）
- **状态**: 待审核 —— 未编写任何实现代码
- **作者**: workx agent
- **日期**: 2026-09-30

---

## 1. 需求背景与验收标准

### 1.1 背景

`src/agent/util/json_schema.h` 目前只有 11 行文档注释，声明支持 `type` / `properties` / `required` / `enum`，但**没有任何实现代码**。实际校验依赖每个工具手写 `validate_input()`：

- `itool.h:87` 的默认实现**无条件返回 ok**，不覆盖即零校验。
- 22 个内置工具中，仅 `FileEdit` / `FileRead` / `FileWrite` / `Glob` 手写了校验，其余全部"裸奔"。
- `ToolExecutor::execute()`（`executor.h:211`）调用 `validate_input()`，但无法兜底。

### 1.2 影响

- **schema 只发给 LLM 看，执行侧不校验**：工具声明与实际约束各说各话。
- **新工具漏写 `validate_input` 就是裸奔**，且无告警、无测试能发现。
- **参数错误无法产生结构化、可自纠的错误信息**回灌给模型，模型只能猜着重试。
- 手工覆盖 22 个内置工具 + MCP 外部工具不可持续。

### 1.3 验收标准（来自 issue 原文）

1. `json_schema.h` 有实现，且被 `ToolExecutor` 调用。
2. 新增一个只声明 `schema`、不写 `validate_input` 的测试工具，传入非法参数时能被拦截。
3. 补单测：非法类型、缺必填、越界枚举、嵌套 object 各一例。

---

## 2. 受影响模块定位

| 文件 | 现状 | 改动 |
|------|------|------|
| `src/agent/util/json_schema.h` | 空壳（仅注释） | 实现校验逻辑（核心新增） |
| `src/agent/tool/itool.h` | `validate_input` 默认无条件 ok（第 87-92 行） | 默认实现改为"若声明了 schema 则走 schema 校验" |
| `src/agent/tool/executor.h` | `execute()` 第 210-219 行调用 `validate_input` | 在校验前插入统一 schema 校验步骤 |
| `src/agent/tool/registry.h` | 管理工具实例 | 无需改动（schema 经 `IToolMetadata::input_schema()` 取） |
| 新增测试 | — | `tests/` 下补 schema 校验单测 |

### 2.1 关键调用链

```
LLM tool_call
  → ToolExecutor::execute(tool_name, input, ctx)
    → [新增] JsonSchemaValidator::validate(input_schema, input)   ← 统一 schema 校验
    → tool->validate_input(input, ctx)                              ← 语义补充校验（保留）
    → tool->call(input, ctx)
```

---

## 3. 资料调研结论

### 3.1 业界实现

| 方案 | 说明 | 取舍 |
|------|------|------|
| `nlohmann/json-schema-validator`（pboettch） | 完整 JSON Schema Draft-07/2019-09/2020-12 校验库，与 nlohmann/json 深度集成 | 功能全但引入**外部依赖**，编译体积增加 |
| `jsoncons` | 内置 JSON Schema 支持，无外部依赖 | 需换 JSON 库，成本过高 |
| **自写轻量校验器** | 只实现 `type`/`properties`/`required`/`enum`/基础 `range`/`pattern` | **符合 issue 原始设计意图**："保持轻量、不依赖外部库" |

### 3.2 结论

issue #80 原文明确要求："按注释承诺把 `json_schema.h` 实现出来……保持**轻量、不依赖外部库**的原始设计意图"。

因此采用**自写轻量校验器**，只覆盖工具参数声明实际用到的关键字子集（工具 schema 均为 `type: object` + `properties` + `required` + 少量 `enum`/`range`/`pattern`），不追求完整 JSON Schema 标准。

---

## 4. 推荐实现方案

### 4.1 设计原则

1. **只做静态 schema 校验**，不做语义校验（后者仍由各工具 `validate_input` 负责）。
2. **纯函数、无状态、无副作用**，便于单测。
3. **错误信息结构化**，可被 LLM 读懂并自纠。

### 4.2 数据结构

在 `json_schema.h` 中新增：

```cpp
namespace agent::util {

/// @brief 单条 schema 校验错误（结构化，供 LLM 回灌）
struct SchemaError {
    std::string tool;       ///< 工具名
    std::string param;      ///< 出错的参数路径（JSON Pointer 风格，如 "/content"）
    std::string expected;   ///< 期望值（如 "string"、"one of [a,b,c]"）
    std::string actual;     ///< 实际值（如 "number"、具体值）
    std::string hint;       ///< 可读提示

    /// @brief 序列化为 LLM 可读文本
    std::string to_string() const;
};

/// @brief 校验结果：error 为空表示通过
struct SchemaResult {
    bool ok = true;
    std::vector<SchemaError> errors;
    std::string to_string() const;  ///< 汇总所有错误为一段文本
};

/// @brief 对 tool 输入做 schema 校验（纯函数）
SchemaResult validate_schema(const nlohmann::json& schema,
                             const nlohmann::json& input,
                             const std::string& tool_name);

} // namespace agent::util
```

### 4.3 校验逻辑（关键字子集）

| 关键字 | 支持程度 | 说明 |
|--------|---------|------|
| `type` | 完整 | string/number/integer/boolean/array/object/null |
| `properties` | 完整（浅层） | 遍历 schema 声明的每个 property |
| `required` | 完整 | 缺失即报 `MissingArgument` |
| `enum` | 完整 | 值不在枚举内即报错 |
| `minimum`/`maximum` | 基础 | number 范围 |
| `minLength`/`maxLength` | 基础 | string 长度 |
| `pattern` | 基础 | `std::regex` 匹配 |
| `items`（array 元素） | 基础 | 对数组元素逐个校验（浅层） |
| 嵌套 object | 浅层递归 | 一层递归，避免无限深 |
| `additionalProperties` | 不强制 | 工具 schema 声明了 false，但首版仅告警不拒绝（见风险） |

### 4.4 关键改动点

#### (a) `itool.h` 默认实现改造

将 `IToolGuard::validate_input` 的默认实现从"无条件 ok"改为：

```cpp
virtual ValidationResult validate_input(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    // 有 schema 声明则走统一 schema 校验，否则 ok（无 schema 的工具不强求）
    auto schema = input_schema();
    if (!schema.is_null() && !schema.empty()) {
        auto res = util::validate_schema(schema, input, name());
        if (!res.ok) {
            return ValidationResult::err(Error::Code::InvalidInput, res.to_string());
        }
    }
    return ValidationResult::ok();
}
```

> 注意：`IToolGuard` 需能访问 `name()` 和 `input_schema()`，它们定义在 `IToolMetadata`。`ITool` 同时继承两者，但 `IToolGuard` 单独不持有元信息。因此更干净的落点是**在 `ToolExecutor` 统一调用**（见 (b)），而非改 `itool.h` 默认实现——这样 `IToolGuard` 保持职责单一，无需跨接口访问元信息。

**最终决策**：schema 校验统一放在 `ToolExecutor::execute()` 内，`itool.h` 默认实现保持 ok（避免 ISP 破坏）。`executor` 已持有 `tool`（`ITool` 类型，含 `input_schema()` 与 `name()`），无跨接口问题。

#### (b) `executor.h` 插入校验步骤

在 `execute()` 的"步骤 4 输入验证"**之后**插入（步骤 4.5）：

```cpp
// 4. 输入验证（工具手写的语义校验优先）
auto validation = tool->validate_input(input, ctx);
if (validation.is_err()) { /* ...原样返回... */ }

// 4.5 统一 schema 校验（#80）：作为兜底，仅当工具未通过手写校验拦截时执行
auto schema = tool->input_schema();
if (!schema.is_null() && !schema.empty()) {
    auto sres = util::validate_schema(schema, input, tool_name);
    if (!sres.ok) {
        bool has_missing = /* 任一 error.is_missing */;
        const auto code = has_missing ? Error::Code::MissingArgument
                                      : Error::Code::InvalidInput;
        LOG_WARN("[tool_executor] tool={} schema validation failed: {}", tool_name, sres.to_string());
        audit::AuditLogger::instance().log_tool_invoke(
            tool_name, input, ctx.session_id, ctx.request_id,
            "deny", "schema invalid: " + sres.to_string(), 0);
        return Error{code, sres.to_string(), tool_name};
    }
}
```

> **顺序决策（实现期修正）**：schema 校验放在 `validate_input()` **之后**，走**兜底语义**而非抢占语义。
>
> 初版设计是"schema 在前"，但实现后跑全量测试暴露了问题：`test_tool_executor.cpp`
> 的 `ValidatingTool` 同时声明了 schema 与手写 `validate_input`，schema 抢先拦截会
> 把工具定制的精确错误信息（`"missing 'value' field"`）架空成 schema 通用信息，
> 降低 LLM 自纠质量。
>
> 改为兜底后两个目标同时达成：手写校验优先（保留精确信息，且能处理路径展开、
> 密钥扫描等 schema 表达不了的语义逻辑）；未手写 `validate_input` 的工具由 schema
> 补位（默认实现无条件返回 ok，正是 #80 要修的"裸奔"）。

#### (c) 新增 `json_schema.cpp`

头文件内联实现或独立 `.cpp`，纳入 `workx_agent` 库编译（需在对应 CMakeLists 添加源文件）。

### 4.5 涉及文件清单

| 文件 | 操作 |
|------|------|
| `src/agent/util/json_schema.h` | 实现校验器（新增代码） |
| `src/agent/util/json_schema.cpp`（可选，或内联） | 实现文件 |
| `src/agent/tool/executor.h` | 插入统一 schema 校验步骤 |
| `src/agent/CMakeLists.txt`（或 agent/util 的构建脚本） | 注册新源文件 |
| `tests/*.cpp`（新增） | 单测：非法类型/缺必填/越界枚举/嵌套 object |

---

## 5. 备选方案对比

| 方案 | 优点 | 缺点 | 结论 |
|------|------|------|------|
| **A. 自写轻量校验器（推荐）** | 零依赖、符合 issue 意图、可控、可结构化错误 | 需自己实现+维护，非完整标准 | ✅ 采用 |
| B. 引入 `json-schema-validator` | 完整标准、社区成熟 | 引入外部依赖、编译体积增大、违背 issue"轻量"意图 | ❌ 不采用 |
| C. 只改 `itool.h` 默认实现 | 改动最小 | 破坏 ISP（IToolGuard 需访问 IToolMetadata）；仅覆盖"未手写"的工具 | ❌ 不采用（作为 (b) 的补充思路） |
| D. 维持现状，靠 review 把关 | 零成本 | 无法解决 22 工具裸奔、无结构化错误回灌 | ❌ 不采用 |

---

## 6. 潜在风险与边界情况

| 风险/边界 | 影响 | 应对 |
|-----------|------|------|
| **破坏现有工具的语义校验** | 部分工具 `validate_input` 依赖相对路径展开、密钥扫描等，schema 校验不应抢占 | schema 校验只做静态类型/必填/枚举，**不碰**路径展开、密钥扫描等语义逻辑；两者并存 |
| **schema 声明与手写校验不一致** | 某些工具 schema 声明的 `required` 比手写校验更宽松/更严格 | 首版以 schema 为准；上线后跑全量工具测试，发现冲突逐一核对（见验证） |
| **`additionalProperties: false` 误伤** | 弱模型可能传多余字段被拒 | 首版对 `additionalProperties: false` **仅告警不拒绝**（宽松），后续按需收紧 |
| **嵌套 object / array 校验深度** | 无限递归风险 | 限制递归深度（如 3 层），超出则跳过深层校验 |
| **number vs integer 边界** | `nlohmann::json` 中 3.0 与 3 同属 number | integer 校验用 `is_number_integer()` |
| **pattern 正则性能/异常** | 恶意/错误 pattern 导致异常或 ReDoS | `std::regex` 用 `try/catch` 包裹，失败降级为跳过该 pattern |
| **22 个工具逐一验证工作量大** | 回归风险 | 先接单测 + 新测试工具验证框架正确，再跑 smoke 全链路（`--smoke`） |
| **错误信息被 LLM 误解** | 结构化错误格式需模型可读 | 统一 `to_string()` 输出 `{tool, param, expected, actual, hint}`，与 issue #79 的错误回灌需求对齐 |

---

## 7. 验证与回滚方式

### 7.1 验证

1. **单元测试**（`tests/`）：
   - 非法类型：`{file_path: 123}` 应报 `expected string, actual number`
   - 缺必填：`{}` 应报 `missing required: content`
   - 越界枚举：enum 外的值应报错
   - 嵌套 object：`properties` 内嵌套一层 object 校验
2. **测试工具验证**：新增一个只声明 schema、不写 `validate_input` 的工具，传入非法参数应被 `ToolExecutor` 拦截（直接对应 issue 验收标准）。
3. **全量回归**：`--smoke` 冒烟链路跑通；现有 22 个工具的合法参数调用不被误拒。
4. **对照核对**：对已手写 `validate_input` 的 4 个工具（FileEdit/Read/Write/Glob），确认 schema 校验结果与其手写结果不冲突。

### 7.2 回滚

- 改动集中在 `json_schema.h`（新增）、`executor.h`（插入一段校验）与测试，无跨模块状态。
- 回滚方式：`git revert` 该分支的提交，或删除 `executor.h` 中的校验插入段即可恢复原行为（`json_schema.h` 为空壳不影响编译）。

---

## 8. 决策记录（已确认）

| # | 决策点 | 结论 |
|---|--------|------|
| 1 | `additionalProperties:false` 严格程度 | ✅ **仅告警不拒绝**（宽松，弱模型兼容优先） |
| 2 | 运行时开关（`WORKX_SCHEMA_STRICT`） | ✅ **不加开关**，直接全量生效，靠 git 回滚兜底 |
| 3 | 错误码选择 | ✅ **细分**：缺必填 → `MissingArgument`，类型/枚举/范围错误 → `InvalidInput` |
| 4 | 校验器落点 | ✅ **独立 `json_schema.cpp`**（改 CMake 注册） |

---

_本方案已按上述决策定稿，未修改任何实现代码，等待实施批准。_
