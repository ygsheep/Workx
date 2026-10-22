/**
 * @file logger.cpp
 * @brief Logger 实现
 */

#include "logger.h"

namespace agent::log {
// G-4：移除 static shared_ptr<Logger> instance 和 once_flag
// 改为 get_instance() 内 static 局部变量（Meyers Singleton）
// ============================================================================
// Logger 实现
// ============================================================================

void Logger::enable_file_output(const std::string& filename, bool enable) {
    std::lock_guard<std::mutex> lock(m_file_mutex);

    if (enable && !m_file_enabled.load(std::memory_order_relaxed)) {
        // 关闭当前文件流（如果已打开）
        if (m_file_stream.is_open()) {
            m_file_stream.close();
        }

        // 创建日志目录
        std::filesystem::path log_path(filename);
        if (log_path.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(log_path.parent_path(), ec);
        }

        // 打开新的日志文件
        m_file_stream.open(filename, std::ios::app);
        if (m_file_stream.is_open()) {
            m_filename = filename;
            m_file_enabled.store(true, std::memory_order_relaxed);

            // 启动写入线程
            if (!m_writer_running.load(std::memory_order_relaxed)) {
                m_writer_running.store(true, std::memory_order_relaxed);
                m_writer_thread = std::thread(&Logger::writer_thread_func, this);
            }
        }
    } else if (!enable && m_file_enabled.load(std::memory_order_relaxed)) {
        // 停止文件输出
        m_file_enabled.store(false, std::memory_order_relaxed);
        m_queue_cv.notify_all();

        if (m_writer_thread.joinable()) {
            m_writer_running.store(false, std::memory_order_relaxed);
            m_queue_cv.notify_all();
            m_writer_thread.join();
        }

        if (m_file_stream.is_open()) {
            m_file_stream.close();
        }
        m_filename.clear();
    }
}

void Logger::log(LogLevel level, const std::string& message, const char* file, int line) {
    if (static_cast<int>(level) < m_level.load(std::memory_order_relaxed)) {
        return;
    }

    // 检查重复日志
    if (is_duplicate(message, file, line)) {
        return;
    }

    // 格式化日志
    std::string formatted = format_message(level, message, file, line);

    // 输出到控制台
    // {
    //     std::lock_guard<std::mutex> lock(m_output_mutex);
    //     auto& stream = (level >= LogLevel::LOG_ERROR) ? std::cerr : std::cout;
    //     stream << formatted << std::endl;
    // }

    // 输出到文件
    if (m_file_enabled.load(std::memory_order_relaxed)) {
        enqueue_message(formatted);
    }
}

bool Logger::is_duplicate(const std::string& message, const char* file, int line) {
    // 使用 std::format 构造键
    std::string key = std::format("{}:{}:{}", file, line, message);

    auto now = std::chrono::steady_clock::now();
    auto now_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(m_duplicate_mutex);

    auto it = m_last_messages.find(key);
    if (it != m_last_messages.end()) {
        if (now_ms - it->second < m_duplicate_window_ms.load(std::memory_order_relaxed)) {
            it->second = now_ms;
            return true;
        }
    }

    m_last_messages[key] = now_ms;
    return false;
}

std::string Logger::format_message(LogLevel level, const std::string& message, const char* file,
                                   int line) const noexcept {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    // 使用 localtime_s (Windows) 或 localtime_r (Linux)
    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t);
#else
    localtime_r(&time_t, &tm_buf);
#endif

    // 使用 stringstream 格式化
    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "." << std::setfill('0') << std::setw(3)
       << ms.count() << "] [" << Detail::to_string(level) << "] [" << Detail::extract_filename(file)
       << ":" << line << "] " << message;
    return "[" + ss.str();
}

void Logger::enqueue_message(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        m_log_queue.push(message);
    }
    m_queue_cv.notify_one();
}

void Logger::writer_thread_func() {
    while (m_writer_running.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(m_queue_mutex);

        m_queue_cv.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return !m_log_queue.empty() || !m_writer_running.load(std::memory_order_relaxed);
        });

        if (!m_writer_running.load(std::memory_order_relaxed)) {
            break;
        }

        // 批量写入
        std::string buffer;
        size_t max_size = m_buffer_size.load(std::memory_order_relaxed);

        while (!m_log_queue.empty() && buffer.size() < max_size) {
            buffer += m_log_queue.front() + "\n";
            m_log_queue.pop();
        }

        lock.unlock();

        if (!buffer.empty()) {
            std::lock_guard<std::mutex> file_lock(m_file_mutex);
            if (m_file_stream.is_open()) {
                m_file_stream << buffer;
                m_file_stream.flush();
                maybe_rotate_locked();
            }
        }
    }
}

void Logger::maybe_rotate_locked() {
    const size_t max_bytes = m_max_size_bytes.load(std::memory_order_relaxed);
    const size_t max_files = m_max_files.load(std::memory_order_relaxed);
    if (max_bytes == 0 || max_files == 0 || m_filename.empty()) return;

    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(m_filename, ec);
    if (ec || size == 0 || static_cast<uintmax_t>(max_bytes) > size) return;

    m_file_stream.close();

    const std::string base = m_filename;
    // 从最旧往最新平移：.k -> .k+1，超出 max_files 的最旧文件被丢弃
    for (size_t i = max_files; i > 1; --i) {
        std::error_code e;
        const std::string dst = base + "." + std::to_string(i);
        const std::string src = base + "." + std::to_string(i - 1);
        std::filesystem::remove(dst, e);
        if (std::filesystem::exists(src, e)) {
            std::filesystem::rename(src, dst, e);
        }
    }

    // 当前文件滚动为 .1，并重新打开新的空文件
    std::error_code e;
    std::filesystem::remove(base + ".1", e);
    std::filesystem::rename(base, base + ".1", e);
    m_file_stream.open(base, std::ios::app);
}

}  // namespace agent::log
