/**
 * @file cache_aware_compactor.h
 * @brief 缓存感知分级压缩器（DS_CACHE_OPTIMIZATION_PLAN 层次 3）
 * @details 替代 ContextCompressor 的死代码实现。核心原则：
 *          1. 钉住前缀（system + 首条 user + 已有摘要），永不折叠 → 保缓存命中
 *          2. 分级触发（soft/snip/compact/force），从轻到重
 *          3. 中段折叠：只摘中段，头尾字节不变
 *          4. 卡死守卫：连续压缩仍超阈值 → 暂停，让前缀 append-only 恢复
 *
 *          参考 Reasonix compact.go 的 4 档水位设计。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>
#include <atomic>
#include <functional>
#include <string>
#include "agent/api/chat_types.h"

namespace agent {

/// @brief 缓存感知分级压缩器
/// @details 有状态（卡死计数器、rewrite_version），非线程安全（调用方须串行调用 maybe_compact）
class CacheAwareCompactor {
   public:
    /// @brief 压缩配置
    struct Config {
        int32_t context_window_tokens = 1'000'000;  ///< 上下文窗口（DeepSeek 默认 1M）

        // 水位比例（对齐 Reasonix）
        float soft_ratio = 0.5f;     ///< 仅发 Notice，不动前缀
        float snip_ratio = 0.6f;     ///< 截短旧 tool_result
        float compact_ratio = 0.8f;  ///< 摘要中段
        float force_ratio = 0.9f;    ///< 强制折叠低价值区

        int32_t tail_token_budget = 16'384;    ///< 尾部保留预算（token）
        int32_t snip_head_lines = 80;          ///< snip 保留头行数
        int32_t snip_tail_lines = 12;          ///< snip 保留尾行数
        int32_t max_consecutive_compacts = 2;  ///< 卡死守卫阈值

        /// DS_CACHE M-1：归档目录（非空时，compact_middle 折叠前将原消息
        /// 序列化追加到 <archive_dir>/<timestamp>.jsonl，摘要消息中标注归档路径）
        std::string archive_dir;
    };

    /// @brief 摘要回调类型（用于注入 LLM 摘要能力）
    /// @param messages 待摘要的消息序列
    /// @return 摘要文本
    using SummarizeFn = std::function<std::string(const std::vector<ChatMessage>&)>;

    /// @brief 暂停事件回调（DS_CACHE H-3：卡死守卫触发/恢复时调用）
    /// @param paused true=卡死触发暂停；false=ratio 回落自愈恢复
    /// @param consecutive_compacts 触发时的连续 compact 次数
    /// @param tokens 触发时的 token 数
    /// @param ratio 触发时的窗口占用比
    /// @param notice 人类可读说明
    /// @details 调用方（ChatSession）据此发布 CompactionPausedEvent 到 EventBus
    using PausedCallback = std::function<void(bool paused, int consecutive_compacts, int32_t tokens,
                                              float ratio, const std::string& notice)>;

    /// @brief 压缩结果动作
    enum class Action {
        None,        ///< 无需压缩
        SoftNotice,  ///< 仅通知，未修改消息
        Snip,        ///< 截短了旧 tool_result
        Compact,     ///< 摘要了中段
        Force,       ///< 强制折叠
        Stuck,       ///< 卡死守卫触发，暂停自动压缩
    };

    /// @brief 压缩结果
    struct Result {
        Action action = Action::None;
        std::vector<ChatMessage> messages;  ///< 压缩后的消息（None 时为空）
        int32_t tokens_before = 0;
        int32_t tokens_after = 0;
        int32_t snipped_count = 0;    ///< snip 截短的消息数
        int32_t compacted_count = 0;  ///< compact 摘要的消息数
        std::string notice;           ///< 人类可读通知
    };

    CacheAwareCompactor() : CacheAwareCompactor(Config{}, SummarizeFn{}) {}

    explicit CacheAwareCompactor(Config cfg) : CacheAwareCompactor(std::move(cfg), SummarizeFn{}) {}

    CacheAwareCompactor(Config cfg, SummarizeFn summarize_fn);

    /// @brief 设置暂停事件回调（H-3：卡死守卫触发/恢复时通知调用方）
    void set_paused_callback(PausedCallback cb) { m_paused_cb = std::move(cb); }

    /// @brief DS_CACHE M-4：注入 LLM 摘要回调（compact 阶段调用）
    /// @details 未注入时 compact_middle 走 mechanical_fold_summary 机械折叠。
    ///          注入后调用 LLM 生成真正摘要；LLM 失败时自动 fallback 到机械折叠。
    ///          必须在首次 maybe_compact 前调用。
    void set_summarize_fn(SummarizeFn fn) { m_summarize_fn = std::move(fn); }

    /// @brief DS_CACHE H-4：更新上下文窗口配置（从 provider preset 注入）
    /// @details 仅更新 context_window_tokens，其他配置保持默认。
    ///          必须在首次 maybe_compact 前调用。
    void set_context_window(int32_t context_window_tokens) {
        m_config.context_window_tokens = context_window_tokens;
    }

    /// @brief DS_CACHE M-1：设置归档目录（compact 折叠前归档原消息）
    /// @details 非空时，compact_middle 会将中段原消息序列化追加到
    ///          <archive_dir>/<timestamp>.jsonl，并在摘要消息中标注归档路径。
    ///          必须在首次 maybe_compact 前调用。
    void set_archive_dir(std::string dir) { m_config.archive_dir = std::move(dir); }

    /// @brief 检查并执行压缩
    /// @param messages 消息列表（会被修改）
    /// @return 压缩结果（含修改后的消息）
    /// @details 在每次 build_request 前调用。根据当前 token 估算选择动作。
    Result maybe_compact(std::vector<ChatMessage>& messages);

    /// @brief 是否处于卡死暂停状态
    bool is_stuck() const { return m_stuck.load(); }

    /// @brief 重置状态（新会话 / clear_history）
    void reset();

    /// @brief 获取当前 rewrite_version（前缀形状追踪用）
    int rewrite_version() const { return m_rewrite_version; }

   private:
    Config m_config;
    SummarizeFn m_summarize_fn;
    PausedCallback m_paused_cb;                  ///< H-3：暂停事件回调
    std::atomic<int> m_consecutive_compacts{0};  ///< 连续 compact 次数
    std::atomic<bool> m_stuck{false};            ///< 卡死暂停标志
    int m_rewrite_version = 0;                   ///< 历史改写版本号

    /// @brief 计算钉住的前缀长度（永不折叠部分）
    /// @details system 消息不在此列表（由 API 单独传）。
    ///          钉住：首条 user 消息（若 < 1500 token）+ 已有摘要消息
    size_t pinned_prefix_len(const std::vector<ChatMessage>& messages) const;

    /// @brief 计算尾部保留起始索引（对齐到非 tool 消息，避免孤儿 tool result）
    size_t tail_start(const std::vector<ChatMessage>& messages) const;

    /// @brief snip 阶段：机械截短旧 tool_result（无 API 调用）
    /// @return 截短的消息数
    int snip_stale_tool_results(std::vector<ChatMessage>& messages, size_t head_end,
                                size_t tail_start_idx);

    /// @brief compact 阶段：摘要中段
    /// @return 摘要后的消息数
    int compact_middle(std::vector<ChatMessage>& messages, size_t pinned_end,
                       size_t tail_start_idx);
};

}  // namespace agent
