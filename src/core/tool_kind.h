/**
 * @file tool_kind.h
 * @brief 工具分类枚举（H-3 从 agent/tool/tool_kind.h 迁移至 core 层）
 * @details 原位于 agent/tool/tool_kind.h，因 core/events/events.h 的 ToolCallEvent
 *          使用 agent::tool::ToolType 导致 core 反向依赖 agent。现将枚举迁移至
 *          core/tool_kind.h，agent 反向 include core，消除分层越界。
 *
 *          保留原 agent::tool 命名空间以避免破坏现有调用方。
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include <string_view>

namespace agent::tool {

/// @brief 工具类型分类（用于 UI 渲染分组）
enum class ToolType {
    ReadFile,   ///< 文件读取
    WriteFile,  ///< 文件写入
    EditFile,   ///< 文件编辑
    Execute,    ///< Shell 命令
    Search,     ///< 搜索（grep/find）
    Agent,      ///< 子代理
    Other       ///< 其他/未知
};

/// @brief 根据工具名推断 ToolType（用于 ToolCallEvent）
/// @details L-1：原位于 chat_session.cpp 匿名命名空间，无法被其他模块或测试复用。
///          现提升为 tool_kind.h/.cpp 的公共纯函数，调用方可直接使用。
/// @param name 工具名（如 "Read"/"Write"/"Bash"/"Grep" 等）
/// @return 对应的 ToolType；未知工具名返回 ToolType::Other
ToolType infer_tool_type(std::string_view name);

}  // namespace agent::tool
