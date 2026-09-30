/**
 * @file skill_tool.h
 * @brief SkillTool — 技能加载工具
 * @details 模型按名称调用已加载的 skill（PromptCommand），获取其完整提示词内容。
 *          对应 example/cc 的 SkillTool（tools/SkillTool/prompt.ts）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/command/inclaude/registry.h"
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief 技能加载工具
/// @note 只读查询 registry，无副作用，可跨线程安全共享。
///       registry 可在注册后注入（set_registry），以适配
///       CommandRegistry 晚于工具注册创建的时序。
class SkillTool : public ITool {
   public:
    explicit SkillTool(std::shared_ptr<command::CommandRegistry> registry);

    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;
    /// @brief 只读工具（仅读取技能元信息，无副作用）
    bool is_read_only() const override { return true; }

    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& ctx) const override;

    /// @brief 注入命令注册表（锁内拷贝 std::function 模式，见 CommandBase setter）
    void set_registry(std::shared_ptr<command::CommandRegistry> registry);

   private:
    mutable std::mutex m_mutex;
    std::shared_ptr<command::CommandRegistry> registry_;
};

}  // namespace agent::tool
