/**
 * @file command.h
 * @brief Command 基类 + 三种命令类型
 * @details CommandBase 抽象基类、PromptCommand（模型执行）、LocalCommand（本地执行）
 * @version 1.0.1
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "types.h"

namespace agent::command {

/// 命令基类 — 对应 CommandBase
/// 所有命令类型的公共接口
class CommandBase {
   public:
    virtual ~CommandBase() = default;

    /// 命令名称（用户通过 /name 调用）
    virtual const std::string& name() const { return name_; }

    /// 命令描述
    virtual const std::string& description() const { return description_; }

    /// 是否启用（动态控制）
    /// @note setter 可能跨线程修改，getter 内加锁拷贝 std::function 后锁外调用，
    ///       避免在持锁状态下回调用户代码导致死锁。
    virtual bool is_enabled() const {
        std::function<bool()> fn_copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            fn_copy = is_enabled_;
        }
        return fn_copy ? fn_copy() : true;
    }

    /// 是否隐藏（不在命令面板显示）
    virtual bool is_hidden() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return is_hidden_;
    }

    /// 是否为用户可调用
    virtual bool is_user_invocable() const { return user_invocable_; }

    /// 是否禁止模型调用
    virtual bool is_model_invocation_disabled() const { return disable_model_invocation_; }

    /// 获取参数提示
    virtual const std::optional<std::string>& argument_hint() const { return argument_hint_; }

    /// 获取命令分类
    virtual const std::string& source() const { return source_; }

    /// 获取加载来源
    virtual LoadSource loaded_from() const { return loaded_from_; }

    /// 获取版本
    virtual const std::optional<std::string>& version() const { return version_; }

    /// 是否立即执行（绕过队列）
    virtual bool is_immediate() const { return immediate_; }

    /// 是否敏感命令（参数脱敏）
    virtual bool is_sensitive() const { return sensitive_; }

    /// 获取使用场景描述
    virtual const std::optional<std::string>& when_to_use() const { return when_to_use_; }

    /// 获取适用上下文（skill frontmatter context 字段）
    virtual const std::optional<std::string>& context() const { return context_; }

    /// 获取关联 agent 声明（空 = 不限 agent）
    virtual const std::optional<std::string>& agent() const { return agent_; }

    /// 获取 PreActivate 钩子命令列表
    virtual const std::vector<std::string>& hooks() const { return hooks_; }

    /// 获取对象式通用 Hook（JSON 字符串数组，Skill 激活时注册到 HookManager）
    virtual const std::vector<std::string>& hooks_json() const { return hooks_json_; }

    /// 获取 conditional 触发路径 glob 列表
    virtual const std::vector<std::string>& paths() const { return paths_; }

    /// 获取命令类型标识
    virtual const std::string& type() const = 0;

    // --- setters（线程安全：跨线程修改时加锁） ---
    void set_is_enabled(std::function<bool()> fn) {
        std::lock_guard<std::mutex> lock(m_mutex);
        is_enabled_ = std::move(fn);
    }
    void set_is_hidden(bool hidden) {
        std::lock_guard<std::mutex> lock(m_mutex);
        is_hidden_ = hidden;
    }
    void set_argument_hint(std::string hint) {
        std::lock_guard<std::mutex> lock(m_mutex);
        argument_hint_ = std::move(hint);
    }
    void set_loaded_from(LoadSource src) {
        std::lock_guard<std::mutex> lock(m_mutex);
        loaded_from_ = src;
    }
    void set_source(std::string src) {
        std::lock_guard<std::mutex> lock(m_mutex);
        source_ = std::move(src);
    }
    void set_user_invocable(bool v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        user_invocable_ = v;
    }
    void set_disable_model_invocation(bool v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        disable_model_invocation_ = v;
    }
    void set_when_to_use(std::string v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        when_to_use_ = std::move(v);
    }
    void set_context(std::string v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        context_ = std::move(v);
    }
    void set_agent(std::string v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        agent_ = std::move(v);
    }
    void set_hooks(std::vector<std::string> v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        hooks_ = std::move(v);
    }
    void set_hooks_json(std::vector<std::string> v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        hooks_json_ = std::move(v);
    }
    void set_paths(std::vector<std::string> v) {
        std::lock_guard<std::mutex> lock(m_mutex);
        paths_ = std::move(v);
    }

   protected:
    CommandBase() = default;
    CommandBase(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description)) {}

    mutable std::mutex m_mutex;  ///< 保护跨线程修改的字段

    std::string name_;
    std::string description_;
    std::string source_{"builtin"};
    LoadSource loaded_from_{LoadSource::Builtin};

    std::optional<std::string> argument_hint_;
    std::optional<std::string> version_;
    std::optional<std::string> when_to_use_;
    std::optional<std::string> context_;   ///< 适用上下文（skill frontmatter）
    std::optional<std::string> agent_;     ///< 关联 agent（空 = 不限）
    std::vector<std::string> hooks_;       ///< PreActivate 钩子命令
    std::vector<std::string> hooks_json_;  ///< 对象式通用 Hook（JSON 字符串数组）
    std::vector<std::string> paths_;  ///< conditional 触发路径 glob（空 = 非 conditional）

    std::function<bool()> is_enabled_;
    bool is_hidden_{false};
    bool user_invocable_{true};
    bool disable_model_invocation_{false};
    bool immediate_{false};
    bool sensitive_{false};
};

/// 提示词命令 — 对应 PromptCommand
/// 需要模型执行的技能命令
class PromptCommand : public CommandBase {
   public:
    using PromptGenerator =
        std::function<std::vector<PromptBlock>(const std::string& args, const CommandContext& ctx)>;

    PromptCommand(std::string name, std::string description)
        : CommandBase(std::move(name), std::move(description)) {}

    const std::string& type() const override {
        static const std::string t{"prompt"};
        return t;
    }

    void set_prompt_generator(PromptGenerator gen) {
        std::lock_guard<std::mutex> lock(m_mutex);
        prompt_generator_ = std::move(gen);
    }

    /// 生成提示词内容
    std::vector<PromptBlock> generate_prompt(const std::string& args,
                                             const CommandContext& ctx) const {
        PromptGenerator gen_copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            gen_copy = prompt_generator_;
        }
        if (!gen_copy) return {};
        return gen_copy(args, ctx);
    }

   private:
    PromptGenerator prompt_generator_;
};

/// 本地命令 — 对应 LocalCommand
/// 纯文本输出的本地命令，无 UI
class LocalCommand : public CommandBase {
   public:
    using CommandCall =
        std::function<CommandResult(const std::string& args, const CommandContext& ctx)>;

    LocalCommand(std::string name, std::string description)
        : CommandBase(std::move(name), std::move(description)) {}

    const std::string& type() const override {
        static const std::string t{"local"};
        return t;
    }

    void set_call(CommandCall call) {
        std::lock_guard<std::mutex> lock(m_mutex);
        call_ = std::move(call);
    }

    /// 执行命令
    CommandResult call(const std::string& args, const CommandContext& ctx) const {
        CommandCall call_copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            call_copy = call_;
        }
        if (!call_copy) return CommandResult::error("Command not implemented");
        return call_copy(args, ctx);
    }

   private:
    CommandCall call_;
};

/// 便捷函数：创建 PromptCommand
inline std::shared_ptr<PromptCommand> make_prompt_command(std::string name,
                                                          std::string description) {
    return std::make_shared<PromptCommand>(std::move(name), std::move(description));
}

/// 便捷函数：创建 LocalCommand
inline std::shared_ptr<LocalCommand> make_local_command(std::string name, std::string description) {
    return std::make_shared<LocalCommand>(std::move(name), std::move(description));
}

}  // namespace agent::command
