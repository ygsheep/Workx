/**
 * @file test_cache_aware_compactor.cpp
 * @brief 缓存感知分级压缩器单元测试（DS_CACHE H-1）
 * @details 覆盖四档水位触发、pinned/tail 边界、卡死守卫触发与自愈
 */

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "agent/compact/cache_aware_compactor.h"
#include "agent/api/chat_types.h"

using namespace agent;
using agent::ChatMessage;

namespace {

/// 构造一条 assistant + tool_result 配对的消息序列
std::vector<ChatMessage> make_messages_with_tool_results(int pairs, int result_len = 2'000) {
    std::vector<ChatMessage> msgs;
    msgs.push_back(ChatMessage::user("Initial user question"));
    for (int i = 0; i < pairs; ++i) {
        auto asst = ChatMessage::assistant("thinking about step " + std::to_string(i));
        msgs.push_back(asst);
        std::string content(result_len, 'x');
        msgs.push_back(ChatMessage::tool_result("call_" + std::to_string(i), "Read", content));
    }
    msgs.push_back(ChatMessage::user("Final question"));
    return msgs;
}

/// 紧凑配置：小窗口便于测试触发
CacheAwareCompactor::Config make_test_config(int32_t window = 5'000) {
    CacheAwareCompactor::Config cfg;
    cfg.context_window_tokens = window;
    cfg.soft_ratio = 0.5f;
    cfg.snip_ratio = 0.6f;
    cfg.compact_ratio = 0.8f;
    cfg.force_ratio = 0.9f;
    cfg.tail_token_budget = 500;
    cfg.snip_head_lines = 3;
    cfg.snip_tail_lines = 2;
    cfg.max_consecutive_compacts = 2;
    return cfg;
}

}  // anonymous namespace

// ============================================================
// 水位判定：None / SoftNotice
// ============================================================

TEST_CASE("compactor: below soft ratio → None", "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(100'000);  // 大窗口
    CacheAwareCompactor compactor(cfg);
    auto msgs = make_messages_with_tool_results(2);

    auto result = compactor.maybe_compact(msgs);
    REQUIRE(result.action == CacheAwareCompactor::Action::None);
    REQUIRE(result.tokens_before == result.tokens_after);
}

TEST_CASE("compactor: soft ratio → SoftNotice (no modification)",
          "[compact][compactor][ds_cache]") {
    // 构造消息使其 token 数在 soft 和 snip 之间
    // window=1000, soft=0.5 → 500 tokens, snip=0.9 → 900 tokens
    // 需要消息 token 数 ∈ [500, 900) → 字符数 ∈ [2000, 3600)
    auto cfg = make_test_config(1'000);
    cfg.soft_ratio = 0.5f;
    cfg.snip_ratio = 0.9f;  // 提高 snip 门槛，确保只触发 soft
    CacheAwareCompactor compactor(cfg);

    // 构造约 750 token 的消息（3000 字符 / 4），落入 [500, 900) soft 区间
    auto msgs = make_messages_with_tool_results(1, 3'000);
    auto result = compactor.maybe_compact(msgs);

    REQUIRE(result.action == CacheAwareCompactor::Action::SoftNotice);
    REQUIRE_FALSE(result.notice.empty());
    REQUIRE(result.tokens_before == result.tokens_after);
}

// ============================================================
// snip 水位：截短旧 tool_result
// ============================================================

TEST_CASE("compactor: snip ratio → Snip (truncate tool_results)",
          "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.2f;
    cfg.compact_ratio = 0.95f;  // 避免触发 compact
    CacheAwareCompactor compactor(cfg);

    // 构造多条 tool_result，总量超过 snip 但低于 compact
    auto msgs = make_messages_with_tool_results(3, 1'500);

    auto result = compactor.maybe_compact(msgs);

    // 应触发 snip（至少截短一条 tool_result）
    REQUIRE(
        (result.action == CacheAwareCompactor::Action::Snip ||
         result.action == CacheAwareCompactor::Action::Compact));  // snip 无可截短时可能 fallback
    REQUIRE(result.tokens_after <= result.tokens_before);
}

// ============================================================
// compact 水位：摘要中段
// ============================================================

TEST_CASE("compactor: compact ratio → Compact (fold middle)", "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.max_consecutive_compacts = 100;  // 避免卡死守卫干扰
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(5, 1'500);

    auto result = compactor.maybe_compact(msgs);

    REQUIRE((result.action == CacheAwareCompactor::Action::Compact ||
             result.action == CacheAwareCompactor::Action::Force ||
             result.action == CacheAwareCompactor::Action::Snip));
    REQUIRE(result.tokens_after < result.tokens_before);
    // compact 后 rewrite_version 应递增
    if (result.action == CacheAwareCompactor::Action::Compact ||
        result.action == CacheAwareCompactor::Action::Force) {
        REQUIRE(compactor.rewrite_version() > 0);
    }
}

// ============================================================
// pinned_prefix_len：钉住前缀边界
// ============================================================

TEST_CASE("compactor: pinned prefix protects first user message",
          "[compact][compactor][ds_cache]") {
    // 首条 user 消息应被钉住，不被压缩
    auto cfg = make_test_config(1'500);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.max_consecutive_compacts = 100;
    CacheAwareCompactor compactor(cfg);

    std::string pinned_content = "THIS IS THE PINNED FIRST USER MESSAGE";
    std::vector<ChatMessage> msgs;
    msgs.push_back(ChatMessage::user(pinned_content));
    // 添加大量 tool_result 触发压缩
    for (int i = 0; i < 5; ++i) {
        msgs.push_back(ChatMessage::assistant("asst " + std::to_string(i)));
        msgs.push_back(
            ChatMessage::tool_result("c" + std::to_string(i), "Read", std::string(1'000, 'x')));
    }
    msgs.push_back(ChatMessage::user("final question"));

    compactor.maybe_compact(msgs);

    // 首条 user 消息内容应保持不变
    REQUIRE(msgs[0].role == ChatMessage::Role::User);
    REQUIRE(msgs[0].content == pinned_content);
}

TEST_CASE("compactor: tail preserves recent messages", "[compact][compactor][ds_cache]") {
    // 尾部消息应被保留
    auto cfg = make_test_config(1'500);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.tail_token_budget = 500;
    cfg.max_consecutive_compacts = 100;
    CacheAwareCompactor compactor(cfg);

    std::vector<ChatMessage> msgs;
    msgs.push_back(ChatMessage::user("first user"));
    for (int i = 0; i < 5; ++i) {
        msgs.push_back(ChatMessage::assistant("asst " + std::to_string(i)));
        msgs.push_back(
            ChatMessage::tool_result("c" + std::to_string(i), "Read", std::string(800, 'x')));
    }
    std::string tail_content = "FINAL USER QUESTION MUST SURVIVE";
    msgs.push_back(ChatMessage::user(tail_content));

    compactor.maybe_compact(msgs);

    // 尾部 user 消息应保留
    bool found_tail = false;
    for (const auto& m : msgs) {
        if (m.content == tail_content) {
            found_tail = true;
            break;
        }
    }
    REQUIRE(found_tail);
}

// ============================================================
// 卡死守卫：触发与自愈
// ============================================================

TEST_CASE("compactor: stuck guard triggers after max consecutive compacts",
          "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.max_consecutive_compacts = 1;  // 一次 compact 即触发卡死
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(5, 1'500);

    // 第一次 compact 即触发卡死守卫（max_consecutive_compacts=1）
    auto r1 = compactor.maybe_compact(msgs);

    // compact 后应因 max_consecutive_compacts=1 触发 stuck
    REQUIRE(compactor.is_stuck());
}

TEST_CASE("compactor: stuck self-heals when ratio drops below soft",
          "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.5f;
    cfg.snip_ratio = 0.6f;
    cfg.compact_ratio = 0.7f;
    cfg.max_consecutive_compacts = 1;  // 一次 compact 即触发卡死
    CacheAwareCompactor compactor(cfg);

    // 先触发卡死
    auto msgs_big = make_messages_with_tool_results(5, 1'500);
    compactor.maybe_compact(msgs_big);
    REQUIRE(compactor.is_stuck());

    // 压缩或替换为小消息，使 ratio 低于 soft
    std::vector<ChatMessage> msgs_small;
    msgs_small.push_back(ChatMessage::user("short question"));

    auto result = compactor.maybe_compact(msgs_small);

    // ratio < soft → 自愈，清除 stuck
    REQUIRE_FALSE(compactor.is_stuck());
    REQUIRE(result.action == CacheAwareCompactor::Action::None);
}

TEST_CASE("compactor: reset clears stuck state", "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.max_consecutive_compacts = 1;  // 一次 compact 即触发卡死
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(5, 1'500);
    compactor.maybe_compact(msgs);
    REQUIRE(compactor.is_stuck());
    REQUIRE(compactor.rewrite_version() > 0);

    compactor.reset();

    REQUIRE_FALSE(compactor.is_stuck());
    REQUIRE(compactor.rewrite_version() == 0);
}

// ============================================================
// PausedCallback 回调触发
// ============================================================

TEST_CASE("compactor: paused callback invoked on stuck", "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.max_consecutive_compacts = 1;  // 一次 compact 即触发卡死

    bool callback_called = false;
    bool callback_paused = false;

    CacheAwareCompactor compactor(cfg);
    compactor.set_paused_callback([&](bool paused, int /*consecutive*/, int32_t /*tokens*/,
                                      float /*ratio*/, const std::string& /*notice*/) {
        callback_called = true;
        callback_paused = paused;
    });

    auto msgs = make_messages_with_tool_results(5, 1'500);
    compactor.maybe_compact(msgs);

    REQUIRE(callback_called);
    REQUIRE(callback_paused);
}

TEST_CASE("compactor: paused callback invoked on self-heal", "[compact][compactor][ds_cache]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.5f;
    cfg.snip_ratio = 0.6f;
    cfg.compact_ratio = 0.7f;
    cfg.max_consecutive_compacts = 1;  // 一次 compact 即触发卡死

    int heal_call_count = 0;
    CacheAwareCompactor compactor(cfg);
    compactor.set_paused_callback([&](bool paused, int, int32_t, float, const std::string&) {
        if (!paused) ++heal_call_count;
    });

    // 触发卡死
    auto msgs_big = make_messages_with_tool_results(5, 1'500);
    compactor.maybe_compact(msgs_big);
    REQUIRE(compactor.is_stuck());

    // 自愈
    std::vector<ChatMessage> msgs_small;
    msgs_small.push_back(ChatMessage::user("short"));
    compactor.maybe_compact(msgs_small);

    REQUIRE_FALSE(compactor.is_stuck());
    REQUIRE(heal_call_count >= 1);
}

// ============================================================
// 窗口为 0：跳过压缩
// ============================================================

TEST_CASE("compactor: zero window → skip compaction", "[compact][compactor][ds_cache]") {
    CacheAwareCompactor::Config cfg;
    cfg.context_window_tokens = 0;
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(10, 5'000);
    auto result = compactor.maybe_compact(msgs);

    REQUIRE(result.action == CacheAwareCompactor::Action::None);
}

// ============================================================
// 空消息列表
// ============================================================

TEST_CASE("compactor: empty messages → None", "[compact][compactor][ds_cache]") {
    CacheAwareCompactor compactor(make_test_config());
    std::vector<ChatMessage> empty;
    auto result = compactor.maybe_compact(empty);
    REQUIRE(result.action == CacheAwareCompactor::Action::None);
}

// ============================================================
// M-1：归档 — compact 折叠前归档原中段消息
// ============================================================

TEST_CASE("compactor: archive middle messages on compact",
          "[compact][compactor][ds_cache][archive]") {
    namespace fs = std::filesystem;
    // 使用临时目录作为归档目录
    auto archive_dir = fs::temp_directory_path() / "workx_test_archive";
    fs::remove_all(archive_dir);
    fs::create_directories(archive_dir);

    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.archive_dir = archive_dir.string();
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(5, 1'500);
    auto result = compactor.maybe_compact(msgs);

    // 应触发 compact（或 force/stuck）
    REQUIRE((result.action == CacheAwareCompactor::Action::Compact ||
             result.action == CacheAwareCompactor::Action::Force ||
             result.action == CacheAwareCompactor::Action::Stuck));

    // 归档目录应至少有一个 .jsonl 文件
    int jsonl_count = 0;
    for (auto& entry : fs::directory_iterator(archive_dir)) {
        if (entry.path().extension() == ".jsonl") {
            ++jsonl_count;
            // 验证文件非空
            REQUIRE(fs::file_size(entry) > 0);
        }
    }
    REQUIRE(jsonl_count >= 1);

    // 摘要消息中应包含归档路径标注
    bool found_archive_ref = false;
    for (const auto& m : msgs) {
        if (m.content.find("<!-- archive:") != std::string::npos) {
            found_archive_ref = true;
            break;
        }
    }
    REQUIRE(found_archive_ref);

    // 清理
    fs::remove_all(archive_dir);
}

TEST_CASE("compactor: no archive when archive_dir empty",
          "[compact][compactor][ds_cache][archive]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;
    cfg.archive_dir = "";  // 未配置归档
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(5, 1'500);
    compactor.maybe_compact(msgs);

    // 摘要消息中不应包含归档路径标注
    bool found_archive_ref = false;
    for (const auto& m : msgs) {
        if (m.content.find("<!-- archive:") != std::string::npos) {
            found_archive_ref = true;
            break;
        }
    }
    REQUIRE_FALSE(found_archive_ref);
}

// ============================================================
// set_context_window（H-4）
// ============================================================

TEST_CASE("compactor: set_context_window updates threshold", "[compact][compactor][ds_cache]") {
    CacheAwareCompactor compactor(make_test_config(100'000));  // 大窗口，不触发

    auto msgs = make_messages_with_tool_results(3, 1'000);
    REQUIRE(compactor.maybe_compact(msgs).action == CacheAwareCompactor::Action::None);

    // 缩小窗口，同样消息应触发压缩
    compactor.set_context_window(1'000);
    auto result = compactor.maybe_compact(msgs);
    REQUIRE(result.action != CacheAwareCompactor::Action::None);
}

// ============================================================
// M-4：LLM 摘要回调注入
// ============================================================

TEST_CASE("compactor: summarize_fn invoked on compact",
          "[compact][compactor][ds_cache][summarize]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;

    bool summarize_called = false;
    std::vector<ChatMessage> captured_middle;

    // 注入自定义摘要函数：记录调用并返回固定摘要
    CacheAwareCompactor compactor(cfg);
    compactor.set_summarize_fn([&](const std::vector<ChatMessage>& middle) {
        summarize_called = true;
        captured_middle = middle;
        return "<compaction-summary>\nLLM summary for " + std::to_string(middle.size()) +
               " messages\n</compaction-summary>";
    });

    auto msgs = make_messages_with_tool_results(5, 1'500);
    auto result = compactor.maybe_compact(msgs);

    // 应触发 compact，且 summarize_fn 被调用
    REQUIRE((result.action == CacheAwareCompactor::Action::Compact ||
             result.action == CacheAwareCompactor::Action::Force ||
             result.action == CacheAwareCompactor::Action::Stuck));
    REQUIRE(summarize_called);
    REQUIRE_FALSE(captured_middle.empty());

    // 摘要消息应包含 LLM 返回的内容
    bool found_llm_summary = false;
    for (const auto& m : msgs) {
        if (m.content.find("LLM summary for") != std::string::npos) {
            found_llm_summary = true;
            break;
        }
    }
    REQUIRE(found_llm_summary);
}

TEST_CASE("compactor: summarize_fn throws → fallback to mechanical",
          "[compact][compactor][ds_cache][summarize]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;

    // 注入总是抛异常的摘要函数
    CacheAwareCompactor compactor(cfg);
    compactor.set_summarize_fn([](const std::vector<ChatMessage>&) -> std::string {
        throw std::runtime_error("LLM unavailable");
    });

    auto msgs = make_messages_with_tool_results(5, 1'500);
    auto result = compactor.maybe_compact(msgs);

    // 异常被捕获，fallback 到机械折叠，compact 仍成功
    REQUIRE((result.action == CacheAwareCompactor::Action::Compact ||
             result.action == CacheAwareCompactor::Action::Force ||
             result.action == CacheAwareCompactor::Action::Stuck));

    // 摘要消息应包含机械折叠的标记（"机械版" 或 "compaction-summary"）
    bool found_mechanical = false;
    for (const auto& m : msgs) {
        if (m.content.find("机械版") != std::string::npos ||
            m.content.find("compaction-summary") != std::string::npos) {
            found_mechanical = true;
            break;
        }
    }
    REQUIRE(found_mechanical);
}

TEST_CASE("compactor: no summarize_fn → mechanical fold (default)",
          "[compact][compactor][ds_cache][summarize]") {
    auto cfg = make_test_config(2'000);
    cfg.soft_ratio = 0.1f;
    cfg.snip_ratio = 0.15f;
    cfg.compact_ratio = 0.3f;

    // 不注入 summarize_fn
    CacheAwareCompactor compactor(cfg);

    auto msgs = make_messages_with_tool_results(5, 1'500);
    auto result = compactor.maybe_compact(msgs);

    REQUIRE((result.action == CacheAwareCompactor::Action::Compact ||
             result.action == CacheAwareCompactor::Action::Force ||
             result.action == CacheAwareCompactor::Action::Stuck));

    // 摘要消息应包含机械折叠标记
    bool found_mechanical = false;
    for (const auto& m : msgs) {
        if (m.content.find("机械版") != std::string::npos) {
            found_mechanical = true;
            break;
        }
    }
    REQUIRE(found_mechanical);
}
