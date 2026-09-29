/**
 * @file tool_kind.cpp
 * @brief 工具分类纯函数实现（L-1：从 chat_session.cpp 匿名命名空间提升）
 * @details 原位于 src/agent/core/chat_session.cpp:32-41 的匿名命名空间内，
 *          无法被其他模块或测试复用。L-1 修复将其移至 core/tool_kind.h/.cpp
 *          作为公共纯函数，调用方可直接 include "core/tool_kind.h" 使用。
 * @version 2.1.0
 * @date 2026-07
 */

#include "core/tool_kind.h"

#include <string_view>

namespace agent::tool {

ToolType infer_tool_type(std::string_view name) {
    if (name == "Read") return ToolType::ReadFile;
    if (name == "Write") return ToolType::WriteFile;
    if (name == "Edit") return ToolType::EditFile;
    if (name == "Bash") return ToolType::Execute;
    if (name == "Grep" || name == "Glob") return ToolType::Search;
    if (name == "Agent") return ToolType::Agent;
    return ToolType::Other;
}

}  // namespace agent::tool
