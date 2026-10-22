/**
 * @file test_input_processor.cpp
 * @brief InputProcessor 单元测试
 * @details 验证 @file 引用保持路径原样（不注入文件内容）、图片附件提取、
 *          多命令拆分等行为。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "agent/input/processor.h"
#include "agent/command/inclaude/registry.h"

using namespace agent::input;
using namespace agent::command;

// ============================================================
// InputProcessor @file 引用（保持路径原样，不注入内容）
// ============================================================

TEST_CASE("InputProcessor keeps @file ref as path, not content", "[input][processor]") {
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry);

    // 执行：用户输入带 @file 引用
    auto result = processor.process("read this @/path/to/foo.txt", CommandContext{});

    // 验证：@file 保持路径原样出现在 messages，不读文件、不产生 <file> 标签
    REQUIRE(result.should_query);
    bool found_ref = false;
    for (const auto& m : result.messages) {
        if (m.find("@/path/to/foo.txt") != std::string::npos) {
            found_ref = true;
        }
        // 旧逻辑的 <file> 内容注入与读取失败提示都不应再出现
        REQUIRE(m.find("<file") == std::string::npos);
        REQUIRE(m.find("[Could not read file:") == std::string::npos);
    }
    REQUIRE(found_ref);
}

TEST_CASE("InputProcessor keeps quoted @file ref as path", "[input][processor]") {
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry);

    auto result = processor.process("check @\"src/main.cpp\"", CommandContext{});

    REQUIRE(result.should_query);
    bool found_ref = false;
    for (const auto& m : result.messages) {
        if (m.find("@src/main.cpp") != std::string::npos) {
            found_ref = true;
        }
        REQUIRE(m.find("<file") == std::string::npos);
    }
    REQUIRE(found_ref);
}

// ============================================================
// InputProcessor 构造（单参数：命令注册表）
// ============================================================

TEST_CASE("InputProcessor is not default constructible", "[input][processor]") {
    static_assert(!std::is_default_constructible_v<InputProcessor>,
                  "InputProcessor must not be default constructible");

    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry);
    (void)processor;
}

// ============================================================
// 图片附件：@path → image_paths（绝对路径）
// ============================================================

TEST_CASE("InputProcessor converts image ref to absolute path", "[input][processor][vision]") {
    // 真实临时图片文件（仅扩展名有意义，processor 不做内容校验）
    namespace fs = std::filesystem;
    auto img_path = fs::temp_directory_path() / "workx_input_test_img.png";
    {
        std::ofstream ofs(img_path, std::ios::binary);
        ofs << "fake-image-bytes";
    }
    struct ScopeGuard {
        fs::path p;
        ~ScopeGuard() { fs::remove(p); }
    } guard{img_path};

    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry);

    // 带引号的绝对路径：@"<abs>"；parser 提取后 processor 校验存在并保留绝对路径
    auto result = processor.process("描述这张图 @\"" + img_path.string() + "\"", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(result.image_paths.size() == 1);
    REQUIRE(result.image_paths[0] == fs::weakly_canonical(img_path).string());
    // 文本保留在 messages
    bool found_text = false;
    for (const auto& m : result.messages) {
        if (m.find("描述这张图") != std::string::npos) found_text = true;
    }
    REQUIRE(found_text);
}

TEST_CASE("InputProcessor reports missing image and skips it", "[input][processor][vision]") {
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry);

    auto result = processor.process("看图 @/nonexistent/workx_missing.png", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(result.image_paths.empty());
    bool found_error = false;
    for (const auto& m : result.messages) {
        if (m.find("[Could not read image:") != std::string::npos) found_error = true;
    }
    REQUIRE(found_error);
}

// ============================================================
// 多命令支持："/skill1 + /skill2" 逐个执行并合并结果
// ============================================================

namespace {

/// @brief 注册一个 PromptCommand（模拟 skill），提示文本为固定字符串
std::shared_ptr<PromptCommand> make_skill_cmd(const std::string& name, const std::string& prompt) {
    auto cmd = make_prompt_command(name, "skill " + name);
    cmd->set_prompt_generator([prompt](const std::string&, const CommandContext&) {
        return std::vector<PromptBlock>{
            PromptBlock{.type = PromptBlockType::Text, .text = prompt, .image = {}}};
    });
    return cmd;
}

}  // namespace

TEST_CASE("InputProcessor executes multiple slash commands joined by +",
          "[input][processor][multi_command]") {
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("skill-001", "PROMPT_A"));
    registry->register_command(make_skill_cmd("skill-002", "PROMPT_B"));
    InputProcessor processor(registry);

    auto result = processor.process("/skill-001 + /skill-002", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE_FALSE(result.is_error);
    // 两个 skill 的提示文本都合并进 output_text
    REQUIRE(result.output_text.find("PROMPT_A") != std::string::npos);
    REQUIRE(result.output_text.find("PROMPT_B") != std::string::npos);
    REQUIRE(result.output_text.find("PROMPT_A") < result.output_text.find("PROMPT_B"));
}

TEST_CASE("InputProcessor single slash command unaffected by + splitting",
          "[input][processor][multi_command]") {
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("skill-001", "PROMPT_A"));
    InputProcessor processor(registry);

    auto result = processor.process("/skill-001", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(result.output_text.find("PROMPT_A") != std::string::npos);
}

TEST_CASE("InputProcessor treats + inside args as non-multi command",
          "[input][processor][multi_command]") {
    auto registry = std::make_shared<CommandRegistry>();
    // 记录被调用的命令名
    std::vector<std::string> called;
    auto cmd = make_prompt_command("skill-001", "skill");
    cmd->set_prompt_generator([&called](const std::string& args, const CommandContext&) {
        called.push_back(args);
        return std::vector<PromptBlock>{
            PromptBlock{.type = PromptBlockType::Text, .text = "PROMPT", .image = {}}};
    });
    registry->register_command(cmd);
    InputProcessor processor(registry);

    // "+ bar" 不是 "/" 开头 → 不按多命令拆分，整串作为单命令参数
    auto result = processor.process("/skill-001 + bar", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(called.size() == 1);
    REQUIRE(called[0] == "+ bar");
}

TEST_CASE("InputProcessor multi command reports error for missing command",
          "[input][processor][multi_command]") {
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("skill-001", "PROMPT_A"));
    InputProcessor processor(registry);

    auto result = processor.process("/skill-001 + /nope", CommandContext{});

    // 存在的命令正常执行，缺失命令报错
    REQUIRE(result.is_error);
    REQUIRE(result.output_text.find("PROMPT_A") != std::string::npos);
    REQUIRE(result.output_text.find("Command not found") != std::string::npos);
}
