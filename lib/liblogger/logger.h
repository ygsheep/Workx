/**
 * @file logger.h
 * @brief 现代C++20轻量级日志系统
 * @details
 * 提供高性能、线程安全的日志记录功能，支持控制台输出和异步文件写入。
 *
 * 主要特性：
 * - 线程安全：使用原子操作和互斥锁保证并发安全
 * - 异步写入：独立线程处理文件I/O，不阻塞主线程
 * - 重复过滤：智能过滤短时间内的重复日志
 * - 格式化支持：使用std::format进行高效格式化
 *
 * @author DearTs Team
 * @date 2025
 * @version 2.0
 *
 * Copyright (c) 2025 DearTs Project. All rights reserved.
 */

#pragma once

#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <format>
#include <chrono>
#include <mutex>
#include <atomic>
#include <fstream>
#include <thread>
#include <queue>
#include <condition_variable>
#include <filesystem>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif

// G-3：命名空间统一为 agent::log（小写），保留 Agent 作为向后兼容 alias
namespace agent::log {

// ============================================================================
// 日志级别
// ============================================================================

/**
 * @brief 日志级别枚举
 * @note 枚举值带 LOG_ 前缀，避免与 Windows 头文件的 ERROR 宏冲突
 */
enum class LogLevel : int {
    LOG_TRACE = 0,  ///< 详细跟踪信息
    LOG_DEBUG = 1,  ///< 调试信息
    LOG_INFO = 2,   ///< 一般信息
    LOG_WARN = 3,   ///< 警告信息
    LOG_ERROR = 4,  ///< 错误信息
    LOG_FATAL = 5   ///< 致命错误信息
};

namespace Detail {

/**
 * @brief 将日志级别转换为字符串
 * @param level 日志级别
 * @return 级别字符串
 */
constexpr const char* to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::LOG_TRACE:
            return "TRACE";
        case LogLevel::LOG_DEBUG:
            return "DEBUG";
        case LogLevel::LOG_INFO:
            return "INFO";
        case LogLevel::LOG_WARN:
            return "WARN";
        case LogLevel::LOG_ERROR:
            return "ERROR";
        case LogLevel::LOG_FATAL:
            return "FATAL";
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief 从路径中提取文件名
 * @param path 文件路径
 * @return 文件名
 */
inline std::string extract_filename(const char* path) noexcept {
    const char* last_slash = path;
    for (const char* p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            last_slash = p + 1;
        }
    }
    return std::string(last_slash);
}

}  // namespace Detail

// ============================================================================
// Logger 核心类
// ============================================================================

/**
 * @brief 日志记录器类
 * @details
 * 单例模式，线程安全的日志记录器。支持控制台输出和异步文件写入。
 *
 * 使用示例：
 * @code
 * auto& logger = DearTs::Logger::get_instance();
 * logger.set_level(DearTs::LogLevel::LOG_DEBUG);
 * logger.enable_file_output("logs/app.log", true);
 * logger.info("Application started");
 * @endcode
 */
class Logger final {  // 单例类，禁止继承
    // 技巧：定义一个简单的结构体，并在构造函数中使用它
    struct Token {
        explicit Token() = default;
    };  // 定义在私有区
   public:
    explicit Logger(Token /*unused*/)
        : m_level(static_cast<int>(LogLevel::LOG_INFO)),
          m_file_enabled(false),
          m_writer_running(false),
          m_buffer_size(4096),
          m_duplicate_window_ms(DEFAULT_DUPLICATE_WINDOW_MS) {}
    /**
     * @brief 析构函数
     */
    ~Logger() {
        // G-2：停止写入线程（join 替代 detach，避免 use-after-free）
        // 顺序：先设置 running=false 唤醒写线程，再 join 等待其处理完队列并退出，
        //       最后关闭文件流。若 detach 则写线程可能在文件流关闭后仍写入。
        if (m_writer_thread.joinable()) {
            m_writer_running.store(false, std::memory_order_relaxed);
            m_queue_cv.notify_all();
            m_writer_thread.join();  // 等待写线程退出（最多阻塞 100ms）
        }

        // 关闭文件流
        {
            std::lock_guard<std::mutex> file_lock(m_file_mutex);
            if (m_file_stream.is_open()) {
                m_file_stream.flush();
                m_file_stream.close();
            }
        }
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    /**
     * @brief 获取Logger单例实例
     * @return Logger引用（G-4：返回引用替代 shared_ptr，单例不应共享所有权）
     */
    static Logger& get_instance() noexcept {
        static Logger instance(Token{});
        return instance;
    }

    // ==================== 配置接口 ====================

    /**
     * @brief 设置日志级别
     * @param level 日志级别
     */
    void set_level(LogLevel level) noexcept {
        m_level.store(static_cast<int>(level), std::memory_order_relaxed);
    }

    /**
     * @brief 获取当前日志级别
     * @return 当前日志级别
     */
    LogLevel get_level() const noexcept {
        return static_cast<LogLevel>(m_level.load(std::memory_order_relaxed));
    }

    /**
     * @brief 启用或禁用文件输出
     * @param filename 日志文件路径
     * @param enable 是否启用文件输出
     */
    void enable_file_output(const std::string& filename, bool enable = true);

    /**
     * @brief 检查是否启用了文件输出
     * @return 是否启用了文件输出
     */
    bool is_file_output_enabled() const noexcept {
        return m_file_enabled.load(std::memory_order_relaxed);
    }

    /**
     * @brief 设置缓冲区大小
     * @param size 缓冲区大小（字节）
     */
    void set_buffer_size(size_t size) noexcept {
        m_buffer_size.store(size, std::memory_order_relaxed);
    }

    /**
     * @brief 设置文件大小轮转
     * @param max_size_bytes 单文件超过该字节数时轮转（0 = 不轮转）
     * @param max_files 保留的历史轮转文件数（workx.log.1 .. workx.log.N）
     */
    void set_rotation(size_t max_size_bytes, size_t max_files) noexcept {
        m_max_size_bytes.store(max_size_bytes, std::memory_order_relaxed);
        m_max_files.store(max_files, std::memory_order_relaxed);
    }

    // ==================== 日志记录接口 ====================

    /**
     * @brief 记录日志消息
     * @param level 日志级别
     * @param message 日志消息
     * @param file 源文件名
     * @param line 行号
     */
    void log(LogLevel level, const std::string& message, const char* file = __builtin_FILE(),
             int line = __builtin_LINE());

    /**
     * @brief 记录TRACE级别日志
     * @param msg 日志消息
     */
    void trace(const std::string& msg, const char* file = __builtin_FILE(),
               int line = __builtin_LINE()) {
        log(LogLevel::LOG_TRACE, msg, file, line);
    }

    /**
     * @brief 记录DEBUG级别日志
     * @param msg 日志消息
     */
    void debug(const std::string& msg, const char* file = __builtin_FILE(),
               int line = __builtin_LINE()) {
        log(LogLevel::LOG_DEBUG, msg, file, line);
    }

    /**
     * @brief 记录INFO级别日志
     * @param msg 日志消息
     */
    void info(const std::string& msg, const char* file = __builtin_FILE(),
              int line = __builtin_LINE()) {
        log(LogLevel::LOG_INFO, msg, file, line);
    }

    /**
     * @brief 记录WARN级别日志
     * @param msg 日志消息
     */
    void warn(const std::string& msg, const char* file = __builtin_FILE(),
              int line = __builtin_LINE()) {
        log(LogLevel::LOG_WARN, msg, file, line);
    }

    /**
     * @brief 记录ERROR级别日志
     * @param msg 日志消息
     */
    void error(const std::string& msg, const char* file = __builtin_FILE(),
               int line = __builtin_LINE()) {
        log(LogLevel::LOG_ERROR, msg, file, line);
    }

    /**
     * @brief 记录FATAL级别日志
     * @param msg 日志消息
     */
    void fatal(const std::string& msg, const char* file = __builtin_FILE(),
               int line = __builtin_LINE()) {
        log(LogLevel::LOG_FATAL, msg, file, line);
    }

   private:
    // ==================== 重复日志过滤 ====================

    /**
     * @brief 检查是否为重复消息
     * @param message 日志消息
     * @param file 源文件名
     * @param line 行号
     * @return 是否为重复消息
     */
    bool is_duplicate(const std::string& message, const char* file, int line);

    // ==================== 日志格式化 ====================

    /**
     * @brief 格式化日志消息
     * @param level 日志级别
     * @param message 日志消息
     * @param file 源文件名
     * @param line 行号
     * @return 格式化后的日志消息
     */
    std::string format_message(LogLevel level, const std::string& message, const char* file,
                               int line) const noexcept;

    // ==================== 文件写入 ====================

    /**
     * @brief 将消息加入写入队列
     * @param message 日志消息
     */
    void enqueue_message(const std::string& message);

    /**
     * @brief 文件写入线程函数
     */
    void writer_thread_func();

    /**
     * @brief 当文件超过轮转阈值时执行轮转（调用方需持有 m_file_mutex）
     */
    void maybe_rotate_locked();

    // ==================== 成员变量 ====================
    // G-4：移除 static shared_ptr<Logger> instance 和 once_flag
    // 改为 get_instance() 内 static 局部变量（Meyers Singleton）

    // 日志级别
    std::atomic<int> m_level{static_cast<int>(LogLevel::LOG_INFO)};
    mutable std::mutex m_output_mutex;

    // 文件输出
    std::atomic<bool> m_file_enabled{false};
    std::string m_filename;
    mutable std::mutex m_file_mutex;
    std::ofstream m_file_stream;

    // 异步写入
    std::queue<std::string> m_log_queue;
    mutable std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::thread m_writer_thread;
    std::atomic<bool> m_writer_running{false};
    std::atomic<size_t> m_buffer_size{4096};
    // 大小轮转：m_max_size_bytes 为滚动阈值（0 禁用），m_max_files 为保留的轮转文件数
    std::atomic<size_t> m_max_size_bytes{0};
    std::atomic<size_t> m_max_files{0};

    // 重复过滤
    static constexpr int DEFAULT_DUPLICATE_WINDOW_MS = 1000;
    std::atomic<int> m_duplicate_window_ms{DEFAULT_DUPLICATE_WINDOW_MS};
    std::unordered_map<std::string, long long> m_last_messages;
    mutable std::mutex m_duplicate_mutex;
};

// ============================================================================
// 便捷访问函数
// ============================================================================

}  // namespace agent::log

// G-3：向后兼容 alias（旧代码使用 Agent::Logger，新代码用 agent::log::Logger）
namespace Agent = agent::log;

// ============================================================================
// 日志宏 - 统一简洁的 API
// ============================================================================

// 简单字符串日志（无需格式化）
// G-4：get_instance() 返回引用，用 . 替代 ->
#define LOG_TRACE_STR(msg) ::agent::log::Logger::get_instance().trace(msg)
#define LOG_DEBUG_STR(msg) ::agent::log::Logger::get_instance().debug(msg)
#define LOG_INFO_STR(msg) ::agent::log::Logger::get_instance().info(msg)
#define LOG_WARN_STR(msg) ::agent::log::Logger::get_instance().warn(msg)
#define LOG_ERROR_STR(msg) ::agent::log::Logger::get_instance().error(msg)
#define LOG_FATAL_STR(msg) ::agent::log::Logger::get_instance().fatal(msg)

// 格式化日志（使用 std::format）
// C++20 __VA_OPT__ 标准写法，兼容新预处理器
// G-3/G-4：宏用 agent::log::Logger 全限定名 + 引用调用
#define LOG_TRACE(fmt, ...) \
    ::agent::log::Logger::get_instance().trace(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))
#define LOG_DEBUG(fmt, ...) \
    ::agent::log::Logger::get_instance().debug(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))
#define LOG_INFO(fmt, ...) \
    ::agent::log::Logger::get_instance().info(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))
#define LOG_WARN(fmt, ...) \
    ::agent::log::Logger::get_instance().warn(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))
#define LOG_ERROR(fmt, ...) \
    ::agent::log::Logger::get_instance().error(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))
#define LOG_FATAL(fmt, ...) \
    ::agent::log::Logger::get_instance().fatal(std::format(fmt __VA_OPT__(, ) __VA_ARGS__))

// 向后兼容宏
#define DEARTS_LOGGER() ::agent::log::Logger::get_instance()
#define DEARTS_LOG_TRACE(msg) ::agent::log::Logger::get_instance().trace(msg)
#define DEARTS_LOG_DEBUG(msg) ::agent::log::Logger::get_instance().debug(msg)
#define DEARTS_LOG_INFO(msg) ::agent::log::Logger::get_instance().info(msg)
#define DEARTS_LOG_WARN(msg) ::agent::log::Logger::get_instance().warn(msg)
#define DEARTS_LOG_ERROR(msg) ::agent::log::Logger::get_instance().error(msg)
#define DEARTS_LOG_FATAL(msg) ::agent::log::Logger::get_instance().fatal(msg)
