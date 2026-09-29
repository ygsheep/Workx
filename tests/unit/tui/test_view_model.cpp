/**
 * @file test_view_model.cpp
 * @brief ViewModel 单元测试（ftxtui 无头逻辑）
 * @details 覆盖全部 15 个 Action 的 apply 语义：流式追加、滚动封口、工具块、
 *          错误/忙碌/权限/退出/提示，以及 active_stream 的复用/新建语义。
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "bridge/action.h"
#include "vm/view_model.h"

using namespace ftxtui;

// ============================================================================
// active_stream / has_active_stream
// ============================================================================

TEST_CASE("ViewModel active_stream creates a new assistant on empty", "[view_model][stream]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.has_active_stream());
    auto& m = vm.active_stream();
    REQUIRE(m.role == MsgRole::Assistant);
    REQUIRE(vm.has_active_stream());
    REQUIRE(vm.messages.size() == 1);
}

TEST_CASE("ViewModel active_stream reuses an unsealed assistant", "[view_model][stream]") {
    ViewModel vm;
    auto& m1 = vm.active_stream();
    m1.text = "abc";
    auto& m2 = vm.active_stream();
    REQUIRE(&m1 == &m2);  // 复用同一节点
    REQUIRE(vm.messages.size() == 1);
}

TEST_CASE("ViewModel active_stream creates a new node after sealing", "[view_model][stream]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionTurnDone{.full_content = "done"}));  // 封口
    REQUIRE_FALSE(vm.has_active_stream());
    auto& next = vm.active_stream();
    REQUIRE(next.text.empty());
    REQUIRE(vm.messages.size() == 2);
}

TEST_CASE("ViewModel has_active_stream false after error message", "[view_model][stream]") {
    ViewModel vm;
    vm.apply(ActionError{.message = "boom"});
    REQUIRE_FALSE(vm.has_active_stream());
    REQUIRE(vm.messages.back().role == MsgRole::Error);
}

// ============================================================================
// ActionAppendMessage
// ============================================================================

TEST_CASE("ViewModel append user message marks user + sealed", "[view_model][append]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionAppendMessage{.role = "user", .text = "hi"}));
    REQUIRE(vm.messages.size() == 1);
    REQUIRE(vm.messages[0].role == MsgRole::User);
    REQUIRE(vm.messages[0].sealed);
    REQUIRE(vm.messages[0].text == "hi");
}

TEST_CASE("ViewModel append assistant message marks assistant + sealed", "[view_model][append]") {
    ViewModel vm;
    vm.apply(ActionAppendMessage{.role = "assistant", .text = "reply"});
    REQUIRE(vm.messages[0].role == MsgRole::Assistant);
    REQUIRE(vm.messages[0].sealed);
}

// ============================================================================
// 流式增量
// ============================================================================

TEST_CASE("ViewModel token delta appends and flips streaming", "[view_model][token]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionTokenDelta{.content_delta = "Hello "}));
    REQUIRE(vm.apply(ActionTokenDelta{.content_delta = "world"}));
    REQUIRE(vm.active_stream().text == "Hello world");
    REQUIRE(vm.active_stream().streaming);
}

TEST_CASE("ViewModel reasoning delta marks reasoned and expands by default",
          "[view_model][reasoning]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = "think"});
    auto& m = vm.messages.back();
    REQUIRE(m.reasoned);
    REQUIRE(m.reasoning_expanded == vm.card_defaults.reasoning_expanded);
    REQUIRE(m.reasoning == "think");
}

TEST_CASE("ViewModel reasoning empty delta does not mark reasoned", "[view_model][reasoning]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = ""});
    REQUIRE_FALSE(vm.messages.back().reasoned);
}

TEST_CASE("ViewModel step done clears streaming on back assistant", "[view_model][step_done]") {
    ViewModel vm;
    vm.apply(ActionTokenDelta{.content_delta = "x"});
    REQUIRE(vm.active_stream().streaming);
    vm.apply(ActionStepDone{});
    REQUIRE_FALSE(vm.active_stream().streaming);
}

// ============================================================================
// ActionTurnDone
// ============================================================================

TEST_CASE("ViewModel turn done fills text when empty and seals", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTurnDone{.full_content = "final",
                            .prompt_tokens = 10,
                            .generated_tokens = 20,
                            .cache_read_input_tokens = 5,
                            .prompt_ms = 100.0,
                            .generation_ms = 200.0,
                            .reasoning_ms = 100.0});
    auto& m = vm.messages.back();
    REQUIRE(m.text == "final");
    REQUIRE(m.sealed);
    REQUIRE_FALSE(m.streaming);
    REQUIRE(m.prompt_tokens == 10);
    REQUIRE(m.generated_tokens == 20);
    REQUIRE(m.cache_read_tokens == 5);
    REQUIRE(m.duration_ms == 300.0);  // 100+200 精确可表示
    REQUIRE(m.reasoning_ms == 100.0);
    REQUIRE(vm.busy == false);
    REQUIRE(vm.total_tokens == 35);
    REQUIRE(vm.sidebar.context_used == 10);
}

TEST_CASE("ViewModel turn done keeps existing streamed text", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTokenDelta{.content_delta = "streamed"});
    vm.apply(ActionTurnDone{.full_content = "final"});
    REQUIRE(vm.messages.back().text == "streamed");
}

TEST_CASE("ViewModel turn done accumulates fine-grained token stats", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTurnDone{.full_content = "a",
                            .prompt_tokens = 10,
                            .generated_tokens = 20,
                            .cache_read_input_tokens = 5,
                            .prompt_cache_hit_tokens = 30,
                            .prompt_cache_miss_tokens = 10});
    vm.apply(ActionTurnDone{.full_content = "b",
                            .prompt_tokens = 4,
                            .generated_tokens = 6,
                            .cache_read_input_tokens = 2,
                            .prompt_cache_hit_tokens = 20,
                            .prompt_cache_miss_tokens = 5});
    // 会话累计：prompt 14 / generated 26 / cache_read 7 / hit 50 / miss 15
    REQUIRE(vm.sidebar.prompt_tokens == 14);
    REQUIRE(vm.sidebar.generated_tokens == 26);
    REQUIRE(vm.sidebar.cache_read_tokens == 7);
    REQUIRE(vm.sidebar.cache_hit_tokens == 50);
    REQUIRE(vm.sidebar.cache_miss_tokens == 15);
    // total_tokens 累计（保留 resume 后历史）：35 + 12 = 47
    REQUIRE(vm.sidebar.total_tokens == 47);
    // context_used 始终为最新一轮 prompt（当前上下文占用）
    REQUIRE(vm.sidebar.context_used == 4);
}

TEST_CASE("ViewModel turn done zero cache stats stay zero", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTurnDone{.full_content = "a", .prompt_tokens = 10, .generated_tokens = 5});
    REQUIRE(vm.sidebar.cache_hit_tokens == 0);
    REQUIRE(vm.sidebar.cache_miss_tokens == 0);
    REQUIRE(vm.sidebar.cache_read_tokens == 0);
    REQUIRE(vm.sidebar.prompt_tokens == 10);
    REQUIRE(vm.sidebar.generated_tokens == 5);
}

// ============================================================================
// 工具调用
// ============================================================================

TEST_CASE("ViewModel begin tool creates running block", "[view_model][tool]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1", .arguments = "{}"}));
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.tool_name == "Read");
    REQUIRE(t.call_id == "c1");
    REQUIRE(t.running);
    REQUIRE(t.expanded);  // 任务开始：工具卡自动展开
    REQUIRE(vm.messages.back().tool_use_ids.size() == 1);
}

TEST_CASE("ViewModel begin tool deduplicates same call_id", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"});
    REQUIRE_FALSE(vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"}));  // 重复
    REQUIRE(vm.messages.back().tool_calls.size() == 1);
}

TEST_CASE("ViewModel end tool marks done and records result", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"});
    REQUIRE(vm.apply(ActionEndTool{.call_id = "c1", .result = "content"}));
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.done);
    REQUIRE_FALSE(t.running);
    REQUIRE(t.result == "content");
    REQUIRE_FALSE(t.is_error);
    REQUIRE_FALSE(t.expanded);  // 成功默认收起
}

TEST_CASE("ViewModel end tool error expands by default", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Bash", .call_id = "c1"});
    vm.apply(ActionEndTool{.call_id = "c1", .result = "err", .is_error = true});
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.is_error);
    REQUIRE(t.expanded);
}

TEST_CASE("ViewModel end tool unknown call_id returns false", "[view_model][tool]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.apply(ActionEndTool{.call_id = "missing", .result = ""}));
}

// ============================================================================
// ActionAgentDone
// ============================================================================

TEST_CASE("ViewModel agent done seals unsealed assistant and fills text",
          "[view_model][agent_done]") {
    ViewModel vm;
    vm.apply(ActionSetBusy{.busy = true});  // 预留 assistant
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    auto& m = vm.messages.back();
    REQUIRE(m.sealed);
    REQUIRE(m.text == "answer");
    REQUIRE(m.duration_ms == 99.0);
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done does not append fallback message when last is sealed",
          "[view_model][agent_done]") {
    ViewModel vm;
    // 正常流式完成路径：StreamDoneEvent 已封口（ActionTurnDone），
    // AgentDoneEvent 只是最终汇总，不得追加第二条（否则回复显示两遍）
    vm.apply(ActionAppendMessage{.role = "user", .text = "q"});
    vm.apply(ActionSetBusy{.busy = true});  // 预留 assistant
    vm.apply(ActionTurnDone{.full_content = "answer", .prompt_ms = 10.0, .generation_ms = 20.0});
    REQUIRE(vm.messages.back().sealed);
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    REQUIRE(vm.messages.size() == 2);  // user + assistant，无第三遍
    REQUIRE(vm.messages.back().role == MsgRole::Assistant);
    REQUIRE(vm.messages.back().text == "answer");
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done fills unsealed assistant without active stream",
          "[view_model][agent_done]") {
    ViewModel vm;
    // 流式事件缺失（异常/丢失）：未封口 assistant 由 AgentDone 补填封口
    vm.apply(ActionAppendMessage{.role = "user", .text = "q"});
    vm.apply(ActionSetBusy{.busy = true});
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    REQUIRE(vm.messages.size() == 2);
    REQUIRE(vm.messages.back().role == MsgRole::Assistant);
    REQUIRE(vm.messages.back().text == "answer");
    REQUIRE(vm.messages.back().sealed);
    REQUIRE(vm.messages.back().duration_ms == 99.0);
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done with empty answer does nothing", "[view_model][agent_done]") {
    ViewModel vm;
    vm.apply(ActionAgentDone{});
    REQUIRE(vm.messages.empty());
}

// ============================================================================
// ActionSetBusy
// ============================================================================

TEST_CASE("ViewModel set busy reserves an assistant node", "[view_model][busy]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionSetBusy{.busy = true}));
    REQUIRE(vm.busy);
    REQUIRE_FALSE(vm.messages.empty());
    REQUIRE_FALSE(vm.messages.back().streaming);
    vm.apply(ActionSetBusy{.busy = false});
    REQUIRE_FALSE(vm.busy);
}

// ============================================================================
// 杂项
// ============================================================================

TEST_CASE("ViewModel permissions updates and deduplicates", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionPermissions{.label = "plan"}));
    REQUIRE(vm.sidebar.permission == "plan");
    REQUIRE_FALSE(vm.apply(ActionPermissions{.label = "plan"}));  // 相同不变化
    REQUIRE(vm.apply(ActionPermissions{.label = "bypass"}));
    REQUIRE(vm.sidebar.permission == "bypass");
}

TEST_CASE("ViewModel mode updates and deduplicates", "[view_model][misc][mode]") {
    ViewModel vm;
    // 默认标准模式
    REQUIRE(vm.sidebar.mode.empty());
    REQUIRE(vm.apply(ActionSetMode{.label = "plan"}));
    REQUIRE(vm.sidebar.mode == "plan");
    REQUIRE_FALSE(vm.apply(ActionSetMode{.label = "plan"}));  // 相同不变化
    REQUIRE(vm.apply(ActionSetMode{.label = "minimal"}));
    REQUIRE(vm.sidebar.mode == "minimal");
    // 权限位独立于模式位（互不干扰）
    REQUIRE(vm.apply(ActionPermissions{.label = "bypass"}));
    REQUIRE(vm.sidebar.permission == "bypass");
    REQUIRE(vm.sidebar.mode == "minimal");
}

TEST_CASE("ViewModel shutdown raises pending_exit", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionShutdown{}));
    REQUIRE(vm.pending_exit);
}

TEST_CASE("ViewModel toast stores prompt echo", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionToast{.text = "command output"}));
    REQUIRE(vm.prompt_echo == "command output");
}

TEST_CASE("ViewModel models loaded is ignored by view model", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.apply(ActionModelsLoaded{.models = {"m1"}}));
}

// ============================================================================
// 综合：消息历史组装
// ============================================================================

TEST_CASE("ViewModel assembles a full multi-turn history", "[view_model][integration]") {
    ViewModel vm;
    vm.apply(ActionAppendMessage{.role = "user", .text = "Q1"});
    vm.apply(ActionSetBusy{.busy = true});
    vm.apply(ActionReasoningDelta{.delta = "reason"});
    vm.apply(ActionTokenDelta{.content_delta = "A1"});
    vm.apply(ActionAgentDone{.final_answer = "A1", .total_tool_calls = 0});
    vm.apply(ActionAppendMessage{.role = "user", .text = "Q2"});

    REQUIRE(vm.messages.size() == 3);
    REQUIRE(vm.messages[0].role == MsgRole::User);
    REQUIRE(vm.messages[0].text == "Q1");
    REQUIRE(vm.messages[1].role == MsgRole::Assistant);
    REQUIRE(vm.messages[1].reasoned);
    REQUIRE(vm.messages[1].text == "A1");
    REQUIRE(vm.messages[1].sealed);
    REQUIRE(vm.messages[2].role == MsgRole::User);
    REQUIRE(vm.messages[2].text == "Q2");
}

// ============================================================================
// 子 Agent 聚合（任务调度 tab）
// ============================================================================

TEST_CASE("ViewModel sub agent progress aggregates by task_id", "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 1, .step_type = "thought"});
    REQUIRE(vm.tabs.sub_agents.size() == 1);
    REQUIRE(vm.tabs.sub_agents[0].task_id == "t1");
    REQUIRE(vm.tabs.sub_agents[0].status == "running");
    REQUIRE(vm.tabs.sub_agents[0].step_number == 1);
    // 第二层独立记录：步骤序列（不混入主转录区）
    REQUIRE(vm.sub_records.size() == 1);
    REQUIRE(vm.sub_records[0].steps.size() == 1);
    REQUIRE(vm.sub_records[0].steps[0].step_type == "thought");
    REQUIRE(vm.messages.empty());

    // 同一 task_id 再次进度：不新增条目，更新步骤
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 2, .step_type = "action"});
    REQUIRE(vm.tabs.sub_agents.size() == 1);
    REQUIRE(vm.tabs.sub_agents[0].step_number == 2);
    REQUIRE(vm.sub_records[0].steps.size() == 2);
    REQUIRE(vm.sub_records[0].steps[1].step_type == "action");

    // 新 task_id：新增条目
    vm.apply(ActionSubAgentProgress{.task_id = "t2", .step_number = 1, .step_type = "thought"});
    REQUIRE(vm.tabs.sub_agents.size() == 2);
    REQUIRE(vm.tabs.sub_agents[1].task_id == "t2");
    REQUIRE(vm.sub_records.size() == 2);
    REQUIRE(vm.messages.empty());
}

TEST_CASE("ViewModel sub agent final step does not append message", "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 1, .step_type = "final"});
    REQUIRE(vm.messages.empty());
    REQUIRE(vm.tabs.sub_agents.empty());  // final 步不聚合，由 Completed 处理
}

TEST_CASE("ViewModel sub agent completed updates status and final answer",
          "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 1, .step_type = "thought"});
    vm.apply(ActionSubAgentCompleted{
        .task_id = "t1", .final_answer = "done", .was_error = false, .duration_ms = 1500.0});
    REQUIRE(vm.tabs.sub_agents.size() == 1);
    REQUIRE(vm.tabs.sub_agents[0].status == "done");
    REQUIRE(vm.tabs.sub_agents[0].duration_ms == 1500.0);
    // 第二层：状态 + 最终答复
    REQUIRE(vm.sub_records.size() == 1);
    REQUIRE(vm.sub_records[0].status == "done");
    REQUIRE(vm.sub_records[0].final_answer == "done");
    REQUIRE(vm.sub_records[0].duration_ms == 1500.0);

    // 失败：状态为 failed
    vm.apply(ActionSubAgentCompleted{.task_id = "t2", .was_error = true, .duration_ms = 500.0});
    REQUIRE(vm.tabs.sub_agents.size() == 2);
    REQUIRE(vm.tabs.sub_agents[1].status == "failed");
    REQUIRE(vm.sub_records.size() == 2);
    REQUIRE(vm.sub_records[1].status == "failed");
}

TEST_CASE("ViewModel sub agent action merges observation into tool card",
          "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1",
                                    .step_number = 1,
                                    .step_type = "thought",
                                    .thought_text = "let me check",
                                    .duration_ms = 100.0});
    vm.apply(ActionSubAgentProgress{.task_id = "t1",
                                    .step_number = 2,
                                    .step_type = "action",
                                    .tool_name = "Read",
                                    .tool_input = "{\"file_path\":\"a.txt\"}"});
    vm.apply(ActionSubAgentProgress{.task_id = "t1",
                                    .step_number = 3,
                                    .step_type = "observation",
                                    .observation = "file content",
                                    .is_error = false});

    REQUIRE(vm.sub_records.size() == 1);
    const auto& rec = vm.sub_records[0];
    // thought + action（observation 已合并到 action，不新增独立步骤）
    REQUIRE(rec.steps.size() == 2);
    REQUIRE(rec.steps[0].step_type == "thought");
    REQUIRE(rec.steps[0].thought_text == "let me check");
    REQUIRE(rec.steps[0].duration_ms == 100.0);
    REQUIRE(rec.steps[0].expanded == true);
    REQUIRE(rec.steps[1].step_type == "action");
    REQUIRE(rec.steps[1].tool_name == "Read");
    REQUIRE(rec.steps[1].tool_input == "{\"file_path\":\"a.txt\"}");
    REQUIRE(rec.steps[1].done == true);
    REQUIRE(rec.steps[1].observation == "file content");
    REQUIRE(rec.steps[1].is_error == false);
    REQUIRE(rec.steps[1].expanded == true);
}

TEST_CASE("ViewModel sub agent observation without action stays standalone",
          "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1",
                                    .step_number = 1,
                                    .step_type = "observation",
                                    .observation = "orphan result",
                                    .is_error = true});
    REQUIRE(vm.sub_records.size() == 1);
    REQUIRE(vm.sub_records[0].steps.size() == 1);
    REQUIRE(vm.sub_records[0].steps[0].step_type == "observation");
    REQUIRE(vm.sub_records[0].steps[0].observation == "orphan result");
    REQUIRE(vm.sub_records[0].steps[0].is_error == true);
    REQUIRE(vm.sub_records[0].steps[0].done == true);
}

// ============================================================================
// 修改追踪（P4：Edit/Write → FileChange）
// ============================================================================

TEST_CASE("ViewModel edit tool tracks FileChange with diff", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = "改用 bar() 计算 y\n"});
    vm.apply(ActionBeginTool{
        .tool_name = "Edit", .call_id = "c1", .arguments = R"JSON({"file_path": "src/main.cpp",
                             "old_string": "auto y = foo();",
                             "new_string": "auto y = bar();"})JSON"});

    REQUIRE(vm.tabs.changes.changes.size() == 1);
    const auto& ch = vm.tabs.changes.changes[0];
    REQUIRE(ch.file_path == "src/main.cpp");
    REQUIRE(ch.old_string == "auto y = foo();");
    REQUIRE(ch.new_string == "auto y = bar();");
    REQUIRE(ch.purpose == "改用 bar() 计算 y");  // reasoning 最后一行
    REQUIRE(ch.msg_index == 0);                  // 工具调用所在消息
    REQUIRE(ch.timestamp > 0);
    REQUIRE(ch.diff.size() == 1);
    REQUIRE(ch.diff[0].kind == agent::DiffKind::Modify);
    REQUIRE(ch.diff[0].text == "auto y = bar();");
    REQUIRE(ch.diff[0].line_no == 1);
    REQUIRE(vm.tabs.changes_open);  // 首个修改自动打开变更记录 tab
}

TEST_CASE("ViewModel write tool tracks FileChange as all Insert", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{
        .tool_name = "Write", .call_id = "c1", .arguments = R"JSON({"file_path": "src/new.cpp",
                             "content": "int main() {\n    return 0;\n}\n"})JSON"});

    REQUIRE(vm.tabs.changes.changes.size() == 1);
    const auto& ch = vm.tabs.changes.changes[0];
    REQUIRE(ch.old_string.empty());  // Write 全量改写，无旧内容
    REQUIRE(ch.new_string == "int main() {\n    return 0;\n}\n");
    REQUIRE(ch.diff.size() == 3);
    REQUIRE(ch.diff[0].kind == agent::DiffKind::Insert);
    REQUIRE(ch.diff[1].kind == agent::DiffKind::Insert);
    REQUIRE(ch.diff[2].kind == agent::DiffKind::Insert);
    REQUIRE(ch.diff[2].line_no == 3);
}

TEST_CASE("ViewModel purpose falls back to new_string first line", "[view_model][file_change]") {
    ViewModel vm;
    // 无 reasoning 直接调工具（R1 回退）
    vm.apply(ActionBeginTool{
        .tool_name = "Edit", .call_id = "c1", .arguments = R"JSON({"file_path": "a.txt",
                             "old_string": "x",
                             "new_string": "y"})JSON"});
    REQUIRE(vm.tabs.changes.changes.size() == 1);
    REQUIRE(vm.tabs.changes.changes[0].purpose == "y");
}

TEST_CASE("ViewModel non-file tools do not track FileChange", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{
        .tool_name = "Read", .call_id = "c1", .arguments = R"JSON({"file_path": "a.txt"})JSON"});
    vm.apply(ActionBeginTool{
        .tool_name = "Bash", .call_id = "c2", .arguments = R"JSON({"command": "ls"})JSON"});
    REQUIRE(vm.tabs.changes.changes.empty());
    REQUIRE_FALSE(vm.tabs.changes_open);
}

TEST_CASE("ViewModel malformed arguments do not track FileChange", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Edit", .call_id = "c1", .arguments = "not json"});
    vm.apply(ActionBeginTool{.tool_name = "Edit",
                             .call_id = "c2",
                             .arguments = R"JSON({"old_string": "x"})JSON"});  // 缺 file_path
    REQUIRE(vm.tabs.changes.changes.empty());
}

TEST_CASE("ViewModel multiple edits accumulate FileChanges", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{
        .tool_name = "Edit",
        .call_id = "c1",
        .arguments = R"JSON({"file_path": "a.txt", "old_string": "1", "new_string": "2"})JSON"});
    vm.apply(ActionBeginTool{
        .tool_name = "Edit",
        .call_id = "c2",
        .arguments = R"JSON({"file_path": "b.txt", "old_string": "3", "new_string": "4"})JSON"});
    REQUIRE(vm.tabs.changes.changes.size() == 2);
    REQUIRE(vm.tabs.changes.changes[0].file_path == "a.txt");
    REQUIRE(vm.tabs.changes.changes[1].file_path == "b.txt");
}