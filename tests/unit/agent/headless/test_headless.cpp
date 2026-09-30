/**
 * @file test_headless.cpp
 * @brief headless（非交互执行模式）单元测试 —— Issue #77
 * @details 覆盖 p0-test-plan §1.2 A 的 HL-01 ~ HL-12：
 *          输出序列化（text / json / stream-json）、退出码语义、权限模式映射、
 *          文本回退链、空任务、无人值守不阻塞、与 #81 git 检查点的耦合。
 *
 *          headless 是评测链路入口，此前零测试且失败模式是「静默输出错内容」，
 *          因此这里对输出内容做逐字断言而非仅看退出码。
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/headless/headless.h"
#include "agent/headless/headless_internal.h"  // @internal 可测件
#include "agent/core/react_loop.h"
#include "agent/tool/context.h"
#include "agent/util/git_checkpoint.h"
#include "core/events/event_bus.h"

#include "helpers/mock_config_manager.h"
#include "helpers/mock_git_repo.h"
#include "helpers/mock_provider.h"
#include "helpers/mock_task_manager.h"

namespace {

using namespace agent;
using namespace agent::test;

/// @brief headless 用例公共夹具
/// @details 构造时把 cwd 切到**非 git** 临时目录 —— 让 GitCheckpoint::capture 失败，
///          避免「改动清单」被追加进输出、破坏逐字断言（git 场景由 HL-12 单独覆盖）。
class HeadlessFixture {
   public:
    MockCompletionProvider provider;
    MockConfigManager cfg;
    MockTaskManager tm;

    HeadlessFixture() {
        saved_cwd_ = std::filesystem::current_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        sandbox_ = std::filesystem::temp_directory_path() /
                   ("workx_headless_ut_" + std::to_string(stamp));

        std::error_code ec;
        std::filesystem::create_directories(sandbox_, ec);
        std::filesystem::current_path(sandbox_, ec);
        // 清掉前序用例可能留下的基线（单例状态跨用例残留）
        agent::util::GitCheckpoint::instance().reset();
    }

    ~HeadlessFixture() {
        agent::util::GitCheckpoint::instance().reset();
        std::error_code ec;
        std::filesystem::current_path(saved_cwd_, ec);
        std::filesystem::remove_all(sandbox_, ec);
    }

    HeadlessFixture(const HeadlessFixture&) = delete;
    HeadlessFixture& operator=(const HeadlessFixture&) = delete;

    /// @brief 排队一个纯文本回复（ReActLoop 将以其作为 final_answer）
    std::shared_ptr<MockStreamReader> queue_text(const std::string& text) {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk(text);
        reader->set_usage(10, 20);
        provider.set_next_reader(reader);
        return reader;
    }

    /// @brief 排队一个工具调用回复
    std::shared_ptr<MockStreamReader> queue_tool_call(const std::string& id, const std::string& name,
                                                      const std::string& input_json) {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk("Let me use the tool.");
        reader->add_tool_use_start(id, name);
        reader->add_tool_use_delta(id, input_json);
        reader->set_usage(15, 25);
        provider.set_next_reader(reader);
        return reader;
    }

    /// @brief 以注入后端执行一次 headless
    HeadlessResult run(const HeadlessOptions& opts) {
        return run_headless_with_provider(cfg, tm, EventBus::instance(), opts, &provider);
    }

   private:
    std::filesystem::path saved_cwd_;  ///< 构造前的 cwd，析构时恢复
    std::filesystem::path sandbox_;    ///< 非 git 沙箱目录（让 GitCheckpoint 捕获失败）
};

/// @brief 构造基础选项
HeadlessOptions make_opts(const std::string& task = "do it",
                          const std::string& format = "text") {
    HeadlessOptions o;
    o.task = task;
    o.output_format = format;
    return o;
}

}  // namespace

// ============================================================================
// HL-01 ~ HL-03：输出格式
// ============================================================================

TEST_CASE_METHOD(HeadlessFixture, "HL-01 text 输出为 final_answer 加换行", "[headless][issue77]") {
    queue_text("hello world");
    const auto r = run(make_opts("say hi", "text"));

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.output == "hello world\n");
}

TEST_CASE_METHOD(HeadlessFixture, "HL-02 json 输出含 result/usage/session_id 等字段",
                 "[headless][issue77]") {
    queue_text("hello");
    const auto r = run(make_opts("say hi", "json"));

    REQUIRE(r.exit_code == 0);
    const auto j = nlohmann::json::parse(r.output);
    REQUIRE(j["result"] == "hello");
    REQUIRE(j.contains("session_id"));
    REQUIRE_FALSE(j["session_id"].get<std::string>().empty());
    REQUIRE(j["usage"]["prompt_tokens"] == 10);
    REQUIRE(j["usage"]["generated_tokens"] == 20);
    REQUIRE(j["was_error"] == false);
    REQUIRE(j["was_interrupted"] == false);
}

TEST_CASE_METHOD(HeadlessFixture, "HL-03 stream-json 输出为逐行可解析 NDJSON",
                 "[headless][issue77]") {
    queue_text("streamed answer");
    const auto r = run(make_opts("say hi", "stream-json"));

    REQUIRE(r.exit_code == 0);

    std::istringstream iss(r.output);
    std::string line;
    std::vector<nlohmann::json> rows;
    while (std::getline(iss, line)) {
        if (line.empty()) continue;
        rows.push_back(nlohmann::json::parse(line));  // 每行必须独立可 parse
    }

    REQUIRE_FALSE(rows.empty());
    REQUIRE(rows.back().contains("result"));  // 末行为 result_json
    REQUIRE(rows.back()["result"] == "streamed answer");
}

// ============================================================================
// HL-04 ~ HL-07：退出码
// ============================================================================

TEST_CASE_METHOD(HeadlessFixture, "HL-04 后端创建失败返回 exit 2", "[headless][issue77]") {
    // 走真实入口：空配置 → 无 provider / remote_url → 参数配置错误
    MockConfigManager empty_cfg;
    MockTaskManager empty_tm;
    const auto r = run_headless(empty_cfg, empty_tm, EventBus::instance(), make_opts());

    REQUIRE(r.exit_code == 2);
    REQUIRE(r.output.find("无法创建后端") != std::string::npos);
}

TEST_CASE_METHOD(HeadlessFixture, "HL-05 未知权限模式返回 exit 2", "[headless][issue77]") {
    auto opts = make_opts();
    opts.permission_mode = "bogus-mode";
    const auto r = run(opts);

    REQUIRE(r.exit_code == 2);
    REQUIRE(r.output.find("未知权限模式") != std::string::npos);
    REQUIRE(r.output.find("bogus-mode") != std::string::npos);
}

TEST_CASE_METHOD(HeadlessFixture, "HL-06 任务失败 was_error 返回 exit 1", "[headless][issue77]") {
    // 不排队任何 reader → submit_completion 返回 nullptr
    // → ThoughtResult::Error（该分支不重试，直接 break）→ was_error
    const auto r = run(make_opts("will fail"));

    REQUIRE(r.exit_code == 1);
}

TEST_CASE_METHOD(HeadlessFixture, "HL-07 被中断 was_interrupted 返回 exit 1", "[headless][issue77]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("partial");
    reader->set_cancel_after(1);  // 消费 1 个 chunk 后流返回 Cancelled
    provider.set_next_reader(reader);

    const auto r = run(make_opts("cancel me"));

    REQUIRE(r.exit_code == 1);
}

// ============================================================================
// HL-08 ~ HL-10：回退链 / 映射 / 边界
// ============================================================================

TEST_CASE("HL-08 result_text 回退链 final_answer → partial_content → error_message",
          "[headless][issue77]") {
    ReActResult r;
    r.final_answer = "F";
    r.partial_content = "P";
    r.error_message = "E";
    r.was_error = true;

    REQUIRE(result_text(r) == "F");

    r.final_answer.clear();
    REQUIRE(result_text(r) == "P");

    r.partial_content.clear();
    REQUIRE(result_text(r) == "E");

    // 回退链以 was_error 为门槛：非错误状态下不回退到 error_message
    r.was_error = false;
    REQUIRE(result_text(r).empty());
}

TEST_CASE("HL-09 四种权限模式映射", "[headless][issue77]") {
    using tool::PermissionMode;

    REQUIRE(parse_permission_mode("") == PermissionMode::Default);
    REQUIRE(parse_permission_mode("default") == PermissionMode::Default);
    REQUIRE(parse_permission_mode("accept-edits") == PermissionMode::AcceptEdits);
    REQUIRE(parse_permission_mode("bypass-permissions") == PermissionMode::BypassPermissions);
    REQUIRE(parse_permission_mode("plan") == PermissionMode::Plan);

    REQUIRE_FALSE(parse_permission_mode("nope").has_value());
}

TEST_CASE_METHOD(HeadlessFixture, "HL-10 空任务不崩溃且走正常流程", "[headless][issue77]") {
    queue_text("ok");
    const auto r = run(make_opts(""));

    REQUIRE(r.exit_code == 0);
    REQUIRE(r.output.find("ok") != std::string::npos);
}

// ============================================================================
// HL-11 ~ HL-12：无人值守 / 跨模块耦合
// ============================================================================

TEST_CASE_METHOD(HeadlessFixture, "HL-11 无人值守：AskUser 被调用时不阻塞",
                 "[headless][issue77]") {
    // headless 的 build_loop 固定传 event_bus=nullptr
    // → AskUserTool 内 ctx.event_bus() 抛 logic_error → 被 ToolExecutor 捕获为工具错误
    // → 不进入 5 分钟等待；错误作为 observation 回灌，循环继续到第 2 轮。
    const std::string ask_json =
        R"({"questions":[{"question":"Pick one?","header":"Pick",)"
        R"("options":[{"label":"A"},{"label":"B"}]}]})";
    queue_tool_call("call_1", "AskUser", ask_json);
    queue_text("done after ask");

    const auto start = std::chrono::steady_clock::now();
    const auto r = run(make_opts("ask me"));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();

    // 若真的等应答，AskUserTool 默认超时是 300000ms
    REQUIRE(elapsed_ms < 10000);
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.output.find("done after ask") != std::string::npos);
}

TEST_CASE_METHOD(HeadlessFixture, "HL-12 git 仓库且有改动时输出改动清单",
                 "[headless][issue77]") {
    MockGitRepo repo("hl12");
    if (!repo.ok()) SKIP("git 不可用，跳过 HL-12");

    REQUIRE(repo.write_file("tracked.txt", "one\n"));
    REQUIRE(repo.commit_all("init"));

    // 切到仓库并丢弃夹具留下的无效基线，让 run 内部重新 capture
    std::error_code ec;
    std::filesystem::current_path(repo.path(), ec);
    agent::util::GitCheckpoint::instance().reset();

    REQUIRE(repo.write_file("tracked.txt", "one\ntwo\n"));  // 制造未提交改动

    queue_text("done");
    const auto r = run(make_opts("change the file", "json"));

    REQUIRE(r.exit_code == 0);
    const auto j = nlohmann::json::parse(r.output);
    REQUIRE(j.contains("git_diff_summary"));
    REQUIRE(j["git_diff_summary"].get<std::string>().find("tracked.txt") != std::string::npos);
}

TEST_CASE_METHOD(HeadlessFixture, "HL-12b 非 git 仓库不写 git_diff_summary 且不报错",
                 "[headless][issue77]") {
    // 夹具已把 cwd 设为非 git 临时目录
    queue_text("done");
    const auto r = run(make_opts("noop", "json"));

    REQUIRE(r.exit_code == 0);
    const auto j = nlohmann::json::parse(r.output);
    REQUIRE_FALSE(j.contains("git_diff_summary"));
}
