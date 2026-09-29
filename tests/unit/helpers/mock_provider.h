/**
 * @file mock_provider.h
 * @brief 测试用 Mock ICompletionProvider + IStreamReader
 * @details 可编程的流式读取器，支持 content/reasoning/tool_use chunk、
 *          token 统计、错误/取消注入。供 test_react_loop / test_chat_session /
 *          test_command_system 等测试复用，替代各文件内的简化版 Mock。
 *
 * 使用示例：
 * @code
 *   using namespace agent::test;
 *   MockCompletionProvider provider;
 *   auto reader = std::make_shared<MockStreamReader>();
 *   reader->add_content_chunk("hello").add_content_chunk(" world");
 *   provider.set_next_reader(reader);
 *   // ... 调用被测代码，submit_completion 会返回 reader
 * @endcode
 */

#pragma once

#include <memory>
#include <vector>
#include <deque>
#include <optional>
#include <functional>
#include <string>
#include <mutex>

#include "agent/api/i_completion_provider.h"
#include "agent/api/i_stream_reader.h"
#include "agent/api/chat_types.h"

namespace agent::test {

/// @brief 可编程的 Mock 流式读取器
///
/// 按预定的 chunk 序列依次返回，最后返回 Complete。
/// 支持注入错误/取消状态、token 统计。
class MockStreamReader : public IStreamReader {
   public:
    /// @brief 添加一个普通内容 chunk
    MockStreamReader& add_content_chunk(std::string delta) {
        StreamChunk chunk;
        chunk.content_delta = std::move(delta);
        chunks_.push_back(chunk);
        return *this;
    }

    /// @brief 添加一个 reasoning chunk
    MockStreamReader& add_reasoning_chunk(std::string delta) {
        StreamChunk chunk;
        chunk.reasoning_delta = std::move(delta);
        chunks_.push_back(chunk);
        return *this;
    }

    /// @brief 添加 tool_use_start chunk
    MockStreamReader& add_tool_use_start(const std::string& id, const std::string& name) {
        StreamChunk chunk;
        chunk.is_tool_use_start = true;
        chunk.tool_use_id = id;
        chunk.tool_name = name;
        chunks_.push_back(chunk);
        return *this;
    }

    /// @brief 添加 tool_use_delta chunk
    MockStreamReader& add_tool_use_delta(const std::string& id, const std::string& json_delta) {
        StreamChunk chunk;
        chunk.is_tool_use_delta = true;
        chunk.tool_use_id = id;
        chunk.tool_input_delta = json_delta;
        chunks_.push_back(chunk);
        return *this;
    }

    /// @brief 设置最终 chunk 的 token 统计
    MockStreamReader& set_usage(int32_t prompt, int32_t generated, int32_t cache_creation = 0,
                                int32_t cache_read = 0) {
        prompt_tokens_ = prompt;
        generated_tokens_ = generated;
        cache_creation_ = cache_creation;
        cache_read_ = cache_read;
        return *this;
    }

    /// @brief 设置错误状态（在消费指定数量的 chunk 后触发 Error）
    MockStreamReader& set_error_at(size_t index) {
        error_at_ = index;
        return *this;
    }

    /// @brief 设置取消状态（在消费指定数量的 chunk 后返回 Cancelled）
    MockStreamReader& set_cancel_after(size_t index) {
        cancel_after_ = index;
        return *this;
    }

    StreamState next(std::function<bool()> should_stop, StreamChunk& out) override {
        // 检查外部取消信号
        if (should_stop && should_stop()) {
            return StreamState::Cancelled;
        }

        if (error_at_.has_value() && consumed_ == error_at_) {
            return StreamState::Error;
        }

        if (cancel_after_.has_value() && consumed_ == cancel_after_) {
            return StreamState::Cancelled;
        }

        if (consumed_ >= chunks_.size()) {
            // 返回最终 chunk，携带 token 统计
            out = StreamChunk{};
            out.is_final = true;
            out.prompt_tokens = prompt_tokens_;
            out.generated_tokens = generated_tokens_;
            out.cache_creation_input_tokens = cache_creation_;
            out.cache_read_input_tokens = cache_read_;
            return StreamState::Complete;
        }

        out = chunks_[consumed_++];
        return StreamState::HasData;
    }

    void cancel() override {}

   private:
    std::vector<StreamChunk> chunks_;
    size_t consumed_ = 0;
    std::optional<size_t> error_at_;
    std::optional<size_t> cancel_after_;
    int32_t prompt_tokens_ = 0;
    int32_t generated_tokens_ = 0;
    int32_t cache_creation_ = 0;
    int32_t cache_read_ = 0;
};

/// @brief Mock ICompletionProvider
///
/// 支持排队多个 reader，按 submit_completion 调用顺序依次返回。
/// 记录 submit/interrupt 调用次数，is_generating 反映队列状态。
///
/// 行为兼容性：readers 为空时 submit_completion 返回 nullptr，
/// 与极简版 MockProvider 行为一致，可安全替换。
class MockCompletionProvider : public ICompletionProvider {
   public:
    /// @brief 排队下一次 submit_completion 返回的 reader
    void set_next_reader(std::shared_ptr<MockStreamReader> reader) {
        std::lock_guard<std::mutex> lock(mutex_);
        readers_.push_back(std::move(reader));
    }

    int submit_count = 0;
    int interrupt_count = 0;

    // 记录最近一次请求的 tools schema（供测试断言子 Agent 工具集过滤）
    nlohmann::json last_tools;
    // 记录最近一次请求的消息（供测试断言子 Agent 初始 system 消息 / 工具观察结果）
    std::vector<ChatMessage> last_messages;

    std::shared_ptr<IStreamReader> submit_completion(const CompletionRequest& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        submit_count++;
        last_tools = request.tools;
        last_messages = request.messages;
        if (readers_.empty()) return nullptr;
        auto reader = readers_.front();
        readers_.pop_front();
        return reader;
    }

    void interrupt() override {
        // L-5：与 submit_count 同锁保护，避免并发中断时计数丢失
        std::lock_guard<std::mutex> lock(mutex_);
        interrupt_count++;
    }

    bool is_generating() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return !readers_.empty();
    }

   private:
    mutable std::mutex mutex_;
    std::deque<std::shared_ptr<MockStreamReader>> readers_;
};

}  // namespace agent::test
