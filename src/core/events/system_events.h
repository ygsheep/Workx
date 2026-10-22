/**
 * @file system_events.h
 * @brief 系统事件类型（H-10：从 events.h 按域拆分）
 * @details 跨切面系统事件：模型加载、后端状态、应用关闭。
 *          订阅方按需 include 本文件，避免引入流式/Agent 事件。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace agent {

/// @brief 模型加载进度
struct ModelLoadEvent {
    std::string model_name;
    float progress = 0.0f;
    bool complete = false;
    std::string error;
};

/// @brief 后端连接状态
struct BackendStatusEvent {
    enum Status { Disconnected, Connecting, Connected, Error } status = Disconnected;
    std::string backend_name;
    std::string error;
};

/// @brief 应用关闭请求
struct ShutdownEvent {
    bool force = false;
};

/// @brief 终端尺寸变更事件
/// @details 由 Terminal 在主循环检测到平台层 resize 信号后发布，
///          订阅方可据此刷新 scroll region、重绘动画组件、重放 DisplayBuffer 等。
struct TerminalResizeEvent {
    int old_width = 0;
    int old_height = 0;
    int new_width = 0;
    int new_height = 0;
};

}  // namespace agent
