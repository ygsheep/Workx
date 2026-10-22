/**
 * @file task_events.h
 * @brief 任务生命周期事件定义
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <memory>

namespace agent {

class Task;

struct TaskStartedEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
};

struct TaskProgressEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    float progress;
    float progress_percent;
};

struct TaskCompletedEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    float duration_ms;
};

struct TaskFailedEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    std::string error_message;
    float duration_ms;
};

struct TaskCancelledEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    float duration_ms;
};

struct TaskOutputEvent {
    std::string task_name;
    std::string line;
};

}  // namespace agent
