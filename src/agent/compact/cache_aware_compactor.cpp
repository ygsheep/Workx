/**
 * @file cache_aware_compactor.cpp
 * @brief 缓存感知分级压缩器实现
 * @details 替代 ContextCompressor 的死代码实现。核心思路：
 *          1. 用 estimate_messages_tokens 估算总 token，按窗口比例触发水位
 *          2. 钉住前缀（首条 user / 已有 <compaction-summary>），永不折叠
 *          3. 中段折叠：snip（机械截短 tool_result）→ compact（LLM 摘要中段）
 *          4. 卡死守卫：连续 compact 仍超阈值则暂停，让前缀重新 append-only
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/compact/cache_aware_compactor.h"
#include "agent/compact/token_count.h"
#include "agent/compact/tool_result_maintainer.h"
#include "liblogger/logger.h"

#include <algorithm>
#include <sstream>
#include <format>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <nlohmann/json.hpp>

namespace agent {

namespace {

/// @brief 判定消息是否为钉住的摘要消息（前一轮 compact 的产物）
/// @details 摘要消息以 <compaction-summary> 包裹，作为 user 角色
bool is_summary_message(const ChatMessage& msg) {
    if (msg.role != ChatMessage::Role::User) return false;
    // 简单子串检测（避免引入完整 XML 解析）
    return msg.content.find("<compaction-summary>") != std::string::npos;
}

/// @brief 单轮 compact 后的摘要文本（机械折叠 fallback）
/// @details 不调用 LLM，按消息角色拼接短摘要。
///          真正的 LLM 摘要由 m_summarize_fn 提供，未注入时用此 fallback。
std::string mechanical_fold_summary(const std::vector<ChatMessage>& msgs, size_t begin,
                                    size_t end) {
    std::ostringstream oss;
    oss << "<compaction-summary>\n";
    oss << "以下为历史对话折叠摘要（机械版，含 " << (end - begin) << " 条消息）：\n";
    for (size_t i = begin; i < end; ++i) {
        const auto& m = msgs[i];
        std::string role_tag;
        switch (m.role) {
            case ChatMessage::Role::User:
                role_tag = "user";
                break;
            case ChatMessage::Role::Assistant:
                role_tag = "assistant";
                break;
            case ChatMessage::Role::Tool:
                role_tag = "tool:" + m.tool_name;
                break;
            default:
                role_tag = "system";
                break;
        }
        std::string preview = m.content.substr(0, std::min<size_t>(m.content.size(), 200));
        oss << "  [" << i << "][" << role_tag << "] " << preview;
        if (m.content.size() > 200) oss << "...";
        oss << "\n";
    }
    oss << "</compaction-summary>";
    return oss.str();
}

/// @brief DS_CACHE M-1：归档中段原消息到 NDJSON 文件
/// @param middle 待归档的消息序列
/// @param archive_dir 归档目录
/// @return 归档文件路径（失败返回空串）
/// @details 文件名格式：<archive_dir>/<YYYYMMDD_HHMMSS>_<millis>.jsonl
///          每行一条 JSON（NDJSON），字段含 role/content/tool_name/tool_call_id
std::string archive_middle(const std::vector<ChatMessage>& middle, const std::string& archive_dir) {
    if (archive_dir.empty() || middle.empty()) return {};

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(archive_dir, ec);
    if (ec) {
        LOG_WARN("[cache_aware_compactor] archive: create_directories failed: {}", ec.message());
        return {};
    }

    // 生成时间戳文件名
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
    std::string filename = std::format("{}_{:03}.jsonl", buf, ms.count());
    fs::path filepath = fs::path(archive_dir) / filename;

    // 序列化为 NDJSON（每行一条消息）
    std::ofstream out(filepath, std::ios::app);
    if (!out.is_open()) {
        LOG_WARN("[cache_aware_compactor] archive: cannot open {}", filepath.string());
        return {};
    }

    for (const auto& msg : middle) {
        nlohmann::json j;
        switch (msg.role) {
            case ChatMessage::Role::User:
                j["role"] = "user";
                break;
            case ChatMessage::Role::Assistant:
                j["role"] = "assistant";
                break;
            case ChatMessage::Role::Tool:
                j["role"] = "tool";
                break;
            default:
                j["role"] = "system";
                break;
        }
        j["content"] = msg.content;
        if (msg.role == ChatMessage::Role::Tool) {
            j["tool_name"] = msg.tool_name;
            j["tool_call_id"] = msg.tool_call_id;
        }
        if (!msg.reasoning_content.empty()) {
            j["reasoning_content"] = msg.reasoning_content;
        }
        out << j.dump() << "\n";
    }
    out.close();

    LOG_INFO("[cache_aware_compactor] archived {} messages to {}", middle.size(),
             filepath.string());
    return filepath.string();
}

}  // anonymous namespace

// ============================================================
// 构造
// ============================================================

CacheAwareCompactor::CacheAwareCompactor(Config cfg, SummarizeFn summarize_fn)
    : m_config(std::move(cfg)), m_summarize_fn(std::move(summarize_fn)) {}

// ============================================================
// reset — 重置状态（新会话）
// ============================================================

void CacheAwareCompactor::reset() {
    m_consecutive_compacts.store(0);
    m_stuck.store(false);
    m_rewrite_version = 0;
}

// ============================================================
// maybe_compact — 主入口
// ============================================================

CacheAwareCompactor::Result CacheAwareCompactor::maybe_compact(std::vector<ChatMessage>& messages) {
    Result result;
    result.tokens_before = compact::estimate_messages_tokens(messages);

    // 窗口为 0（未配置）则跳过压缩
    if (m_config.context_window_tokens <= 0) {
        result.action = Action::None;
        return result;
    }

    const float ratio = static_cast<float>(result.tokens_before) /
                        static_cast<float>(m_config.context_window_tokens);

    // H-3：卡死自愈 — ratio 回落到 soft 以下时清除 m_stuck，让前缀重新 append-only 后恢复压缩
    if (m_stuck.load() && ratio < m_config.soft_ratio) {
        m_stuck.store(false);
        m_consecutive_compacts.store(0);
        LOG_INFO(
            "[cache_aware_compactor] stuck self-healed: ratio={:.2f} < soft={:.2f}, "
            "resuming auto-compaction",
            ratio, m_config.soft_ratio);
        if (m_paused_cb) {
            m_paused_cb(false, 0, result.tokens_before, ratio,
                        "compactor self-healed: ratio dropped below soft, resuming");
        }
    }

    // 卡死守卫：暂停自动压缩（自愈未触发时）
    if (m_stuck.load()) {
        result.action = Action::Stuck;
        result.notice = "compactor stuck: auto-compaction paused (consecutive compact limit hit)";
        result.tokens_after = result.tokens_before;
        return result;
    }

    // 水位判定（从轻到重）
    if (ratio < m_config.soft_ratio) {
        result.action = Action::None;
        result.tokens_after = result.tokens_before;
        return result;
    }

    if (ratio < m_config.snip_ratio) {
        // soft 水位：仅 Notice，不动前缀
        result.action = Action::SoftNotice;
        result.notice = std::format("cache soft notice: tokens={} ratio={:.2f} (soft={:.2f})",
                                    result.tokens_before, ratio, m_config.soft_ratio);
        result.tokens_after = result.tokens_before;
        LOG_INFO("[cache_aware_compactor] soft notice, tokens={}, ratio={:.2f}",
                 result.tokens_before, ratio);
        return result;
    }

    // snip / compact / force 都需要先计算头尾边界
    const size_t pinned_end = pinned_prefix_len(messages);
    const size_t tail_idx = tail_start(messages);

    // 边界检查：尾部必须严格在 pinned 之后，且至少留 2 条可折叠消息
    if (tail_idx <= pinned_end + 1) {
        // 没有可折叠的中段，直接进入 compact 的卡死路径
        LOG_WARN("[cache_aware_compactor] no foldable middle, pinned={}, tail={}", pinned_end,
                 tail_idx);
        // 跳过 snip，直接尝试 compact
    } else {
        // snip 水位：机械截短旧 tool_result
        if (ratio < m_config.compact_ratio) {
            int snipped = snip_stale_tool_results(messages, pinned_end, tail_idx);
            if (snipped > 0) {
                result.action = Action::Snip;
                result.snipped_count = snipped;
                result.tokens_after = compact::estimate_messages_tokens(messages);
                result.notice =
                    std::format("snip: {} stale tool_results truncated, tokens {} -> {}", snipped,
                                result.tokens_before, result.tokens_after);
                LOG_INFO("[cache_aware_compactor] snip, count={}, tokens {} -> {}", snipped,
                         result.tokens_before, result.tokens_after);
                // snip 不破坏前缀字节级稳定（只改中段 tool_result 内容，但保留消息位置）
                // 不递增 m_rewrite_version（钉住前缀未变）
                return result;
            }
            // snip 无可截短对象，继续往下走 compact
        }
    }

    // compact / force 水位：摘要中段
    // L-1：原条件 `ratio < force_ratio || ratio >= compact_ratio` 恒真（因前面已过滤 < snip_ratio）
    //      实际语义是"进入 compact 阶段"，直接去掉冗余条件
    {
        // 前置检查：必须有可折叠的中段
        if (tail_idx > pinned_end + 1) {
            int compacted = compact_middle(messages, pinned_end, tail_idx);
            if (compacted > 0) {
                result.action = (ratio >= m_config.force_ratio) ? Action::Force : Action::Compact;
                result.compacted_count = compacted;
                result.tokens_after = compact::estimate_messages_tokens(messages);
                // compact 改写了中段，递增 rewrite_version
                ++m_rewrite_version;
                m_consecutive_compacts.fetch_add(1);

                result.notice = std::format(
                    "compact: {} messages folded, tokens {} -> {}, rewrite_version={}", compacted,
                    result.tokens_before, result.tokens_after, m_rewrite_version);
                LOG_INFO(
                    "[cache_aware_compactor] compact, folded={}, tokens {} -> {}, "
                    "consecutive={}, version={}",
                    compacted, result.tokens_before, result.tokens_after,
                    m_consecutive_compacts.load(), m_rewrite_version);

                // H-3：卡死守卫 — 连续 compact 达阈值，触发暂停事件
                if (m_consecutive_compacts.load() >= m_config.max_consecutive_compacts) {
                    m_stuck.store(true);
                    result.action = Action::Stuck;
                    result.notice += " | stuck guard triggered: auto-compaction paused";
                    LOG_WARN(
                        "[cache_aware_compactor] stuck guard triggered after compact, "
                        "pausing auto-compaction");
                    if (m_paused_cb) {
                        m_paused_cb(true, m_consecutive_compacts.load(), result.tokens_after, ratio,
                                    result.notice);
                    }
                }
                return result;
            }
        }
        // compact 未能折叠任何消息
        LOG_WARN("[cache_aware_compactor] compact produced no fold, tokens={}, ratio={:.2f}",
                 result.tokens_before, ratio);
    }

    // 兜底：未触发任何动作
    result.action = Action::None;
    result.tokens_after = result.tokens_before;
    return result;
}

// ============================================================
// pinned_prefix_len — 钉住前缀长度
// ============================================================

size_t CacheAwareCompactor::pinned_prefix_len(const std::vector<ChatMessage>& messages) const {
    // system 消息不在 messages 中（由 API 单独传），跳过；
    // 这里从第一条非 system 消息开始扫描
    size_t pinned = 0;
    bool saw_first_user = false;

    // 钉住首条 user 消息：若 token 数 < min(1500, 窗口 * 0.15) 则钉住
    const int32_t pin_budget =
        std::min<int32_t>(1500, static_cast<int32_t>(m_config.context_window_tokens * 0.15f));

    for (size_t i = 0; i < messages.size(); ++i) {
        const auto& msg = messages[i];

        // 跳过 system（理论上不在 messages，但容错）
        if (msg.role == ChatMessage::Role::System) {
            pinned = i + 1;
            continue;
        }

        // 首条 user 消息：若小则钉住
        if (!saw_first_user && msg.role == ChatMessage::Role::User) {
            saw_first_user = true;
            int32_t t = compact::estimate_message_tokens(msg);
            if (t <= pin_budget) {
                pinned = i + 1;
                continue;
            }
            // 首条 user 过大，不钉住，后续也不再钉任何 user
            break;
        }

        // 已有摘要消息：钉住（这是上一轮 compact 的产物，是稳定前缀的一部分）
        if (is_summary_message(msg)) {
            pinned = i + 1;
            continue;
        }

        // 其它消息（assistant / tool / 普通 user）：不钉住
        break;
    }

    return pinned;
}

// ============================================================
// tail_start — 尾部保留起始索引
// ============================================================

size_t CacheAwareCompactor::tail_start(const std::vector<ChatMessage>& messages) const {
    if (messages.empty()) return 0;

    int32_t budget = m_config.tail_token_budget;
    if (budget <= 0) budget = 16'384;

    // 从尾部往前累积 token 直到预算用尽
    size_t idx = messages.size();
    int32_t acc = 0;
    while (idx > 0) {
        int32_t t = compact::estimate_message_tokens(messages[idx - 1]);
        if (acc + t > budget && idx < messages.size()) {
            break;  // 预算用尽，停止
        }
        acc += t;
        --idx;
    }

    // 对齐到非 tool 消息：避免尾部首条为 tool（孤儿 tool result）
    // tool 消息必须紧跟其对应的 assistant tool_use，否则 OpenAI/Anthropic 都会报错
    while (idx < messages.size() && messages[idx].role == ChatMessage::Role::Tool) {
        ++idx;
    }

    // 保证至少保留最后一条消息（极端情况）
    if (idx >= messages.size()) {
        idx = messages.size() > 0 ? messages.size() - 1 : 0;
    }

    return idx;
}

// ============================================================
// snip_stale_tool_results — 机械截短旧 tool_result
// ============================================================

int CacheAwareCompactor::snip_stale_tool_results(std::vector<ChatMessage>& messages,
                                                 size_t head_end, size_t tail_start_idx) {
    if (head_end >= tail_start_idx || tail_start_idx > messages.size()) {
        return 0;
    }

    // 使用 tool_result_maintainer 的 snip 策略
    // 按 tool_name 选择策略（只读 vs 副作用）
    int snipped = 0;
    for (size_t i = head_end; i < tail_start_idx && i < messages.size(); ++i) {
        auto& msg = messages[i];
        if (msg.role != ChatMessage::Role::Tool) continue;
        // 跳过空内容或已截短过的消息（含 [snipped ...] 标记）
        if (msg.content.empty()) continue;
        if (msg.content.find("[snipped") != std::string::npos) continue;

        compact::SnipStrategy strategy = compact::select_strategy_by_tool_name(msg.tool_name);
        compact::snip_tool_result(msg, strategy);
        ++snipped;
    }
    return snipped;
}

// ============================================================
// compact_middle — 摘要中段
// ============================================================

int CacheAwareCompactor::compact_middle(std::vector<ChatMessage>& messages, size_t pinned_end,
                                        size_t tail_start_idx) {
    if (pinned_end >= tail_start_idx || tail_start_idx > messages.size()) {
        return 0;
    }

    // 收集中段消息
    std::vector<ChatMessage> middle(messages.begin() + pinned_end,
                                    messages.begin() + tail_start_idx);

    if (middle.empty()) return 0;

    // 调用注入的 LLM 摘要函数；未注入则用机械折叠
    std::string summary_text;
    if (m_summarize_fn) {
        try {
            summary_text = m_summarize_fn(middle);
        } catch (const std::exception& e) {
            LOG_WARN("[cache_aware_compactor] summarize_fn threw: {}, fallback to mechanical",
                     e.what());
            summary_text = mechanical_fold_summary(messages, pinned_end, tail_start_idx);
        }
    } else {
        summary_text = mechanical_fold_summary(messages, pinned_end, tail_start_idx);
    }

    // DS_CACHE M-1：归档原中段消息（archive_dir 非空时），保证可追溯
    std::string archive_path;
    if (!m_config.archive_dir.empty()) {
        archive_path = archive_middle(middle, m_config.archive_dir);
        if (!archive_path.empty()) {
            // 在摘要文本末尾追加归档路径标注
            summary_text +=
                std::format("\n<!-- archive: {} ({} messages) -->", archive_path, middle.size());
        }
    }

    // 构造摘要 user 消息（带 <compaction-summary> 标签，便于后续轮次识别为 pinned）
    ChatMessage summary_msg = ChatMessage::user(summary_text);

    // 原地替换：擦除 [pinned_end, tail_start_idx)，在 pinned_end 插入摘要消息
    messages.erase(messages.begin() + pinned_end, messages.begin() + tail_start_idx);
    messages.insert(messages.begin() + pinned_end, summary_msg);

    LOG_INFO("[cache_aware_compactor] compact_middle: folded {} messages into 1 summary{}",
             middle.size(),
             archive_path.empty() ? "" : std::format(", archived to {}", archive_path));
    return static_cast<int>(middle.size());
}

}  // namespace agent
