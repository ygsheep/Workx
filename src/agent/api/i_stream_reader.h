/**
 * @file i_stream_reader.h
 * @brief 流式读取器接口
 * @details IStreamReader 用于从后端增量读取流式响应
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <functional>
#include <string>
#include "agent/api/chat_types.h"
#include "core/export.h"

namespace agent {

/// @brief 流式读取状态
enum class StreamState {
    HasData,   ///< 有新数据可读
    Complete,  ///< 流式完成
    Error,     ///< 发生错误
    Cancelled  ///< 被取消
};

/// @brief 流式读取器接口
/// @details 从后端增量读取响应，阻塞等待下一个 chunk
class WORKX_API IStreamReader {
   public:
    virtual ~IStreamReader() = default;

    /// @brief 读取下一个 chunk（阻塞）
    /// @param should_stop 外部取消检查，返回 true 时立即停止
    /// @param out 输出的 StreamChunk
    /// @return 当前流状态
    virtual StreamState next(std::function<bool()> should_stop, StreamChunk& out) = 0;

    /// @brief 取消流式读取
    virtual void cancel() = 0;

    /// @brief #89：终止时的 HTTP 状态码
    /// @details 供上层重试与降级判定使用（此前错误码在流层被丢弃，上层只能拿到固定串，
    ///          导致 4xx 与 5xx 无法区分、一律被判为可重试）。
    ///          0 表示无 HTTP 响应（curl 网络错误 / 连接超时 / 本地失败）。
    ///          默认实现返回 0，不强制既有实现改动。
    virtual int http_status() const { return 0; }

    /// @brief #89：终止时的错误信息（正常结束为空）
    virtual std::string error_message() const { return {}; }
};

}  // namespace agent
