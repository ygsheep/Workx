#include "agent/api/remote/http_client.h"
#include "agent/api/remote/sse_stream_reader.h"
#include "agent/api/remote/ssrf.h"

// 平台网络头：ssrf_opensocket_cb 用到 sockaddr_in/sockaddr_in6、ntohl、
// IPPROTO_TCP、socket() 等。Windows 由 winsock 提供，POSIX 由 netinet/sys 提供，
// 必须显式包含（curl/curl.h 不保证传递这些符号，Linux 裸编译会报未声明）。
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // winsock/windows 的 min/max 宏会干扰 std::numeric_limits
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <curl/curl.h>

#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>
#include <array>

#include "liblogger/logger.h"

namespace agent {

// ============================================================
// Curl 全局初始化
// ============================================================

static struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
} s_curl_global;

// ============================================================
// H-1：CURLSH 连接共享（跨 HttpClient 实例复用 TCP/TLS 连接）
// ============================================================
//
// libcurl 的 CURLM 内部已有连接缓存（同一 CURLM 内的 CURL 句柄复用连接），
// 但跨 CURLM 实例（即跨 HttpClient 实例）不共享。CURLSH 提供
// CURL_LOCK_DATA_CONNECT 共享数据，让多个 CURLM/CURL 句柄复用连接缓存。
//
// 线程安全：CURLSH 本身不是线程安全的，通过 lock/unlock 回调加锁。
// 共享数据按 CURL_LOCK_DATA 分组，每组一把 mutex，减少锁争用。

namespace {

/// @brief CURLSH 锁回调使用的 mutex 数组
/// @details 索引按 CURL_LOCK_DATA 枚举值。CURL_LOCK_DATA_CONNECT 是最常用的，
///          其他（如 COOKIE、SSL_SESSION）当前未共享，但数组预留足够空间。
struct CurlShareLocks {
    static constexpr int kLockDataCount = 10;
    std::array<std::mutex, kLockDataCount> mutexes;

    static CurlShareLocks& instance() {
        static CurlShareLocks inst;
        return inst;
    }
};

void curl_share_lock_cb(CURL* /*handle*/, curl_lock_data data, curl_lock_access /*access*/,
                        void* /*userptr*/) {
    int idx = static_cast<int>(data);
    if (idx >= 0 && idx < CurlShareLocks::kLockDataCount) {
        CurlShareLocks::instance().mutexes[idx].lock();
    }
}

void curl_share_unlock_cb(CURL* /*handle*/, curl_lock_data data, void* /*userptr*/) {
    int idx = static_cast<int>(data);
    if (idx >= 0 && idx < CurlShareLocks::kLockDataCount) {
        CurlShareLocks::instance().mutexes[idx].unlock();
    }
}

/// @brief 获取全局共享 CURLSH 句柄
/// @details 共享 CURL_LOCK_DATA_CONNECT（连接缓存），跨 HttpClient 实例复用 TCP/TLS 连接
CURLSH* shared_curl_share() {
    static CURLSH* sh = []() {
        CURLSH* s = curl_share_init();
        if (s) {
            curl_share_setopt(s, CURLSHOPT_LOCKFUNC, curl_share_lock_cb);
            curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, curl_share_unlock_cb);
            curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
            LOG_INFO("[http] CURLSH 连接共享已启用（CURL_LOCK_DATA_CONNECT）");
        }
        return s;
    }();
    return sh;
}

/// @brief 限制允许的协议（含重定向），兼容新旧 curl
/// @details libcurl 7.85.0 起 CURLOPT_PROTOCOLS/REDIR_PROTOCOLS 被弃用，
///          改用 *_STR 字符串形式；按编译期版本选择，避免 -Werror 下
///          -Wdeprecated-declarations 升级为错误导致 Linux 编译失败。
void restrict_allowed_protocols(CURL* curl) {
#if LIBCURL_VERSION_NUM >= 0x075500  // 7.85.0
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
}

}  // anonymous namespace

// ============================================================
// URL 解析
// ============================================================

ParsedUrl HttpClient::parse_url(const std::string& url) {
    // H-4：完全依赖 CURLU API（libcurl 7.62.0+），删除自定义 fallback
    // 旧实现维护两套解析逻辑可能产生不一致结果，统一为单套
    ParsedUrl result;

    CURLU* hurl = curl_url();
    if (!hurl) {
        LOG_ERROR("[http] parse_url '{}' curl_url_init failed", url);
        return result;  // 返回空 ParsedUrl，调用方检测 scheme.empty()
    }

    CURLUcode rc = curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0);
    if (rc != CURLUE_OK) {
        LOG_ERROR("[http] parse_url '{}' failed: {}", url, curl_url_strerror(rc));
        curl_url_cleanup(hurl);
        return result;  // scheme 保持空，调用方检测
    }

    auto get_part = [&](CURLUPart p) -> std::string {
        char* c = nullptr;
        if (curl_url_get(hurl, p, &c, 0) == CURLUE_OK && c) {
            std::string s(c);
            curl_free(c);
            return s;
        }
        return {};
    };
    result.scheme = get_part(CURLUPART_SCHEME);
    result.host = get_part(CURLUPART_HOST);
    result.port = get_part(CURLUPART_PORT);
    if (result.port.empty()) result.port = (result.scheme == "https") ? "443" : "80";
    {
        std::string p = get_part(CURLUPART_PATH);
        if (p.empty()) p = "/";
        std::string q = get_part(CURLUPART_QUERY);
        result.target = q.empty() ? p : p + "?" + q;
    }
    curl_url_cleanup(hurl);
    return result;
}

// ============================================================
// 写入回调（同步 GET）
// ============================================================

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

/// @brief 响应头回调：收集响应头（键转小写，供 Content-Type / Mcp-Session-Id 读取）
static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    auto* headers = static_cast<std::vector<std::pair<std::string, std::string>>*>(userdata);
    const size_t total = size * nitems;
    std::string line(buffer, total);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto start = value.find_first_not_of(" \t");
        if (start != std::string::npos) value = value.substr(start);
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        headers->emplace_back(std::move(key), std::move(value));
    }
    return total;
}

// ============================================================
// SSRF 连接钩子（#25）：建立连接前拦截内网/回环/链路本地目标
// ============================================================
// 通过 CURLOPT_OPENSOCKETFUNCTION 在 libcurl 真正 connect 前拿到解析后的
// sockaddr，命中内网地址返回 CURL_SOCKET_BAD 让 curl 以连接失败中止。
// 相比"仅预检输入 URL"，本钩子同时覆盖 3xx 重定向后的最终目标。
static curl_socket_t ssrf_opensocket_cb(void* /*clientp*/, curlsocktype /*purpose*/,
                                        struct sockaddr* addr) {
    if (!addr) return CURL_SOCKET_BAD;
    if (addr->sa_family == AF_INET) {
        auto* a = reinterpret_cast<struct sockaddr_in*>(addr);
        if (is_private_ipv4(ntohl(a->sin_addr.s_addr))) return CURL_SOCKET_BAD;
    } else if (addr->sa_family == AF_INET6) {
        auto* a = reinterpret_cast<struct sockaddr_in6*>(addr);
        if (is_private_ipv6(a->sin6_addr.s6_addr)) return CURL_SOCKET_BAD;
    }
    return socket(addr->sa_family, SOCK_STREAM, IPPROTO_TCP);
}

// ============================================================
// Sync GET（V2-2：返回 ResultV2<HttpResponse>）
// ============================================================

ResultV2<HttpResponse> HttpClient::get(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    int timeout_ms) {
    LOG_DEBUG("[http] GET {} timeout={}ms", url, timeout_ms);
    // #25 P3-1：SSRF 预检（防御纵深）——开启防护时先解析 URL，命中内网直接拒绝，
    // 提供比连接钩子更清晰的错误信息（连接钩子仍兜底重定向后的最终目标）
    if (m_block_private_ips) {
        const ParsedUrl parsed = parse_url(url);
        if (!parsed.scheme.empty() && !parsed.host.empty() &&
            host_resolves_to_private(parsed.host)) {
            LOG_WARN("[http] GET {} 目标解析到内网/回环/链路本地地址，SSRF 防护拒绝", url);
            return ResultV2<HttpResponse>::err(
                Error::Code::PermissionDenied,
                "SSRF 防护：目标主机解析到内网/回环/链路本地地址，已拒绝请求", url);
        }
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("[http] GET {} curl_easy_init failed", url);
        return ResultV2<HttpResponse>::err(Error::Code::InternalError, "curl_easy_init failed",
                                           url);
    }

    std::string body;
    std::vector<std::pair<std::string, std::string>> resp_headers;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    // C.7：补连接超时，防止 DNS/TCP 阶段挂死耗尽总超时
    // 默认 10s 连接超时（若 timeout_ms < 10s 则跟随总超时）
    long connect_to = (std::min)(10000L, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_to);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // H-1：关联 CURLSH 共享连接缓存，跨 HttpClient 实例复用 TCP/TLS 连接
    if (auto* sh = shared_curl_share()) {
        curl_easy_setopt(curl, CURLOPT_SHARE, sh);
    }
    // #25：SSRF 防护（可选开启）——连接钩子拦截内网目标，并限制重定向协议
    if (m_block_private_ips) {
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, ssrf_opensocket_cb);
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, nullptr);
        restrict_allowed_protocols(curl);
    }

    struct curl_slist* hl = nullptr;
    for (const auto& [k, v] : headers) hl = curl_slist_append(hl, (k + ": " + v).c_str());
    if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        // V2-2：网络错误通过 Error 携带错误码
        std::string err_msg = curl_easy_strerror(rc);
        LOG_ERROR("[http] GET {} failed: {} (rc={})", url, err_msg, static_cast<int>(rc));
        if (hl) curl_slist_free_all(hl);
        curl_easy_cleanup(curl);
        return ResultV2<HttpResponse>::err(Error::from_curl_code(static_cast<int>(rc), url));
    }

    HttpResponse resp;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    resp.status_code = static_cast<unsigned int>(code);
    resp.body = std::move(body);
    resp.headers = std::move(resp_headers);
    if (code >= 400) {
        LOG_WARN("[http] GET {} returned HTTP {} body_len={}", url, code, resp.body.size());
    } else {
        LOG_DEBUG("[http] GET {} -> {} bytes={}", url, code, resp.body.size());
    }
    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return ResultV2<HttpResponse>::ok(std::move(resp));
}

// ============================================================
// Sync POST（同步 body，返回 HttpResponse）
// ============================================================

ResultV2<HttpResponse> HttpClient::post(
    const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body, int timeout_ms) {
    LOG_DEBUG("[http] POST {} body_len={} timeout={}ms", url, body.size(), timeout_ms);
    // #25 P3-1：SSRF 预检（防御纵深），与 get() 一致
    if (m_block_private_ips) {
        const ParsedUrl parsed = parse_url(url);
        if (!parsed.scheme.empty() && !parsed.host.empty() &&
            host_resolves_to_private(parsed.host)) {
            LOG_WARN("[http] POST {} 目标解析到内网/回环/链路本地地址，SSRF 防护拒绝", url);
            return ResultV2<HttpResponse>::err(
                Error::Code::PermissionDenied,
                "SSRF 防护：目标主机解析到内网/回环/链路本地地址，已拒绝请求", url);
        }
    }
    CURL* curl = curl_easy_init();
    if (!curl) {
        LOG_ERROR("[http] POST {} curl_easy_init failed", url);
        return ResultV2<HttpResponse>::err(Error::Code::InternalError, "curl_easy_init failed",
                                           url);
    }

    std::string out_body;
    std::vector<std::pair<std::string, std::string>> resp_headers;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    long connect_to = (std::min)(10000L, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_to);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    if (auto* sh = shared_curl_share()) {
        curl_easy_setopt(curl, CURLOPT_SHARE, sh);
    }
    // #25：SSRF 防护（可选开启）——连接钩子拦截内网目标，并限制重定向协议
    if (m_block_private_ips) {
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETFUNCTION, ssrf_opensocket_cb);
        curl_easy_setopt(curl, CURLOPT_OPENSOCKETDATA, nullptr);
        restrict_allowed_protocols(curl);
    }

    struct curl_slist* hl = nullptr;
    for (const auto& [k, v] : headers) hl = curl_slist_append(hl, (k + ": " + v).c_str());
    if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        // V2-2：网络错误通过 Error 携带错误码
        std::string err_msg = curl_easy_strerror(rc);
        LOG_ERROR("[http] POST {} failed: {} (rc={})", url, err_msg, static_cast<int>(rc));
        if (hl) curl_slist_free_all(hl);
        curl_easy_cleanup(curl);
        return ResultV2<HttpResponse>::err(Error::from_curl_code(static_cast<int>(rc), url));
    }

    HttpResponse resp;
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    resp.status_code = static_cast<unsigned int>(code);
    resp.body = std::move(out_body);
    resp.headers = std::move(resp_headers);
    if (code >= 400) {
        LOG_WARN("[http] POST {} returned HTTP {} body_len={}", url, code, resp.body.size());
    } else {
        LOG_DEBUG("[http] POST {} -> {} bytes={}", url, code, resp.body.size());
    }
    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return ResultV2<HttpResponse>::ok(std::move(resp));
}

// ============================================================
// StreamSession — 异步流式 POST 会话
// ============================================================

class StreamSession : public std::enable_shared_from_this<StreamSession> {
   public:
    CURL* easy_handle() const { return m_curl; }

    StreamSession(const ParsedUrl& url,
                  const std::vector<std::pair<std::string, std::string>>& headers,
                  const std::string& body, std::shared_ptr<SSEStreamReader> reader,
                  std::function<void()> on_complete, const int timeout_ms,
                  const bool block_private_ips)
        : m_body(body),
          m_reader(std::move(reader)),
          m_on_complete(std::move(on_complete))
          // H-2：总时长超时（Timer-based），默认 120 秒
          // timeout_ms 作为空闲超时（LOW_SPEED_TIME），总时长超时独立配置
          // 当 timeout_ms > 0 时，总时长 = max(timeout_ms, 120000) 确保不短于空闲超时
          ,
          m_total_timeout_ms(timeout_ms > 0 ? (std::max)(timeout_ms, 120000) : 120000),
          m_start_time(std::chrono::steady_clock::now()) {
        m_curl = curl_easy_init();
        if (!m_curl) return;

        std::string full_url = url.scheme + "://" + url.host;
        if ((url.scheme == "https" && url.port != "443") ||
            (url.scheme == "http" && url.port != "80"))
            full_url += ":" + url.port;
        full_url += url.target;

        curl_easy_setopt(m_curl, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, m_body.c_str());
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(m_body.size()));
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &StreamSession::stream_write_cb);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
        // 流式传输用空闲超时（无数据传输 N 秒则断开），不用总时长超时
        // 长响应（如 reasoning model 60s+）正常，但断网 N 秒会触发断开
        // CURLOPT_LOW_SPEED_LIMIT + CURLOPT_LOW_SPEED_TIME:
        //   若传输速度低于 LOW_SPEED_LIMIT bytes/s 持续 LOW_SPEED_TIME 秒，则断开
        // 用括号包裹 std::max 防止 windows.h 的 max 宏展开
        const long idle_seconds = (std::max)(1L, static_cast<long>(timeout_ms / 1000));
        curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_TIME, idle_seconds);
        // 保留连接超时（10 秒）
        curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);
        // H-1：关联 CURLSH 共享连接缓存
        if (auto* sh = shared_curl_share()) {
            curl_easy_setopt(m_curl, CURLOPT_SHARE, sh);
        }
        // #25 P2-1：SSRF 防护（可选开启）——流式会话同样拦截内网目标并限制协议
        if (block_private_ips) {
            curl_easy_setopt(m_curl, CURLOPT_OPENSOCKETFUNCTION, ssrf_opensocket_cb);
            curl_easy_setopt(m_curl, CURLOPT_OPENSOCKETDATA, nullptr);
            restrict_allowed_protocols(m_curl);
        }

        for (const auto& [k, v] : headers)
            m_header_list = curl_slist_append(m_header_list, (k + ": " + v).c_str());
        if (m_header_list) curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_header_list);
    }

    ~StreamSession() {
        if (m_curl) {
            // 防御：m_multi 可能已被 HttpClient 析构清理为 nullptr
            // （shutdown 路径会先 curl_multi_cleanup 再触发 StreamSession 析构）
            if (m_added_to_multi.load() && m_multi) {
                curl_multi_remove_handle(m_multi, m_curl);
            }
            curl_easy_cleanup(m_curl);
        }
        if (m_header_list) curl_slist_free_all(m_header_list);
    }

    void run(CURLM* multi) {
        if (!m_curl) {
            finish("curl init failed");
            return;
        }
        m_multi = multi;
        m_added_to_multi.store(true);
        curl_multi_add_handle(m_multi, m_curl);
    }

    /// @brief 立即取消会话：设置标志 + 从 multi 移除 handle，避免依赖下一次 write 回调
    /// @details 原实现只设原子标志，需等下次 write 回调返回 0 才真正停止；
    ///          若网络已断开 write 回调不再触发，cancel 不生效。
    ///          现在直接 remove_handle 让 libcurl 立即停止传输。
    void cancel() {
        m_cancelled.store(true);
        if (m_added_to_multi.load() && m_multi) {
            curl_multi_remove_handle(m_multi, m_curl);
            m_added_to_multi.store(false);
        }
    }

    /// @brief H-2：检查总时长超时
    /// @return true 表示已超时，应由 poll_loop 触发 cancel + finish_with_error
    bool is_total_timeout() const {
        if (m_total_timeout_ms <= 0) return false;  // 0 表示禁用
        auto elapsed = std::chrono::steady_clock::now() - m_start_time;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        return ms > m_total_timeout_ms;
    }

    void on_transfer_done(const CURLcode code) {
        if (m_cancelled.load()) {
            LOG_DEBUG("[http][stream] transfer cancelled");
            finish("");
            return;
        }
        if (code != CURLE_OK) {
            std::string err = curl_easy_strerror(code);
            LOG_ERROR("[http][stream] transfer failed: {} (rc={})", err, static_cast<int>(code));
            finish(err);
            return;
        }

        // 检查 HTTP 状态码
        long http_code = 0;
        curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            // 4xx/5xx：附加已累积的响应体作为错误信息一部分
            // （stream_write_cb 在 http_code >= 400 时累积到 m_error_body 而非喂给 reader）
            std::string err_msg = std::string("HTTP error: ") + std::to_string(http_code);
            if (!m_error_body.empty()) {
                // 截断防止过长（最多 500 字符）
                err_msg += " - " + m_error_body.substr(0, 500);
            }
            LOG_WARN("[http][stream] HTTP {} error_body_len={}", http_code, m_error_body.size());
            // #89：状态码单独传给 reader，不再只存在于字符串里（上层需据此判定重试/降级）
            finish(http_code, err_msg);
            return;
        }
        LOG_DEBUG("[http][stream] transfer done HTTP {}", http_code);
        if (m_reader && !m_reader->is_finished()) m_reader->finish();
        if (m_on_complete) m_on_complete();
    }

    // 主动以错误完成会话（供 HttpClient::async_post_stream 在 curl 初始化失败时调用）
    void finish_with_error(const std::string& err) { finish(err); }

   private:
    void finish(const std::string& err) const {
        if (m_reader && !m_reader->is_finished()) m_reader->finish(err);
        if (m_on_complete) m_on_complete();
    }

    void finish(long http_code, const std::string& err) const {
        if (m_reader && !m_reader->is_finished()) m_reader->finish(http_code, err);
        if (m_on_complete) m_on_complete();
    }

    static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<StreamSession*>(userdata);
        if (self->m_cancelled.load()) return 0;

        // HTTP 错误响应（4xx/5xx）：累积 body 到 m_error_body 作为错误信息
        // 正常响应（2xx）：喂给 reader 进行 SSE 解析
        long http_code = 0;
        curl_easy_getinfo(self->m_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            self->m_error_body.append(static_cast<const char*>(ptr), size * nmemb);
            return size * nmemb;
        }

        if (self->m_reader)
            // C.10：直接传 string_view，避免每个 SSE chunk 都构造 std::string 拷贝
            self->m_reader->feed_data(
                std::string_view(static_cast<const char*>(ptr), size * nmemb));

        // 数据接收进度：每 256KB 记录一次（避免高频日志）
        const size_t received =
            self->m_bytes_received.fetch_add(size * nmemb, std::memory_order_relaxed) +
            size * nmemb;
        if (received / (256 * 1024) != (received - size * nmemb) / (256 * 1024)) {
            LOG_INFO("[http][stream] received_bytes={}KB", received / 1024);
        }
        return size * nmemb;
    }

    CURL* m_curl = nullptr;
    CURLM* m_multi = nullptr;
    std::atomic<bool> m_added_to_multi{false};
    struct curl_slist* m_header_list = nullptr;
    std::string m_body;
    std::string m_error_body;  // 4xx/5xx 响应体累积
    std::shared_ptr<SSEStreamReader> m_reader;
    std::function<void()> m_on_complete;
    std::atomic<bool> m_cancelled{false};
    std::atomic<size_t> m_bytes_received{0};  // 数据接收累计字节（进度日志用）
    // H-2：总时长超时（Timer-based），0 表示禁用
    int m_total_timeout_ms = 120000;  // 默认 2 分钟
    std::chrono::steady_clock::time_point m_start_time;
};

// ============================================================
// HttpClient::Impl
// ============================================================

struct HttpClient::Impl {
    CURLM* multi;
    std::mutex sessions_mutex;
    std::unordered_map<CURL*, std::shared_ptr<StreamSession>> sessions_by_handle;
    std::unordered_map<SSEStreamReader*, std::weak_ptr<StreamSession>> sessions_by_reader;
    std::thread poll_thread;
    std::atomic<bool> running{true};

    Impl() {
        multi = curl_multi_init();
        poll_thread = std::thread([this]() { poll_loop(); });
    }

    void poll_loop() {
        while (running.load()) {
            // issue #15-B: 线程函数必须捕获所有异常，否则 std::terminate → abort()
            // 与 Task::execute / ThreadPool::worker_loop 保持一致的异常防御
            try {
                int still_running = 0;
                curl_multi_perform(multi, &still_running);

                int msgs_left = 0;
                CURLMsg* msg;
                while ((msg = curl_multi_info_read(multi, &msgs_left)) != nullptr) {
                    if (msg->msg == CURLMSG_DONE) {
                        std::shared_ptr<StreamSession> session;
                        {
                            std::lock_guard<std::mutex> lock(sessions_mutex);
                            auto it = sessions_by_handle.find(msg->easy_handle);
                            if (it != sessions_by_handle.end()) {
                                session = it->second;
                                sessions_by_handle.erase(it);
                            }
                            // 也从 reader map 清理（C.4：完善 sessions_by_reader 清理路径）
                            // expired 的 weak_ptr 在此统一回收，避免长生命周期下 map 无限增长
                            for (auto rit = sessions_by_reader.begin();
                                 rit != sessions_by_reader.end();) {
                                if (rit->second.expired())
                                    rit = sessions_by_reader.erase(rit);
                                else
                                    ++rit;
                            }
                        }
                        if (session) session->on_transfer_done(msg->data.result);
                    }
                }

                // H-2：总时长超时检查
                // 遍历所有活跃 session，若超时则 cancel + finish_with_error
                // 注意：必须在 sessions_mutex 锁外调用 finish_with_error，避免
                // on_complete 回调内再次访问 HttpClient 造成死锁
                {
                    std::vector<std::shared_ptr<StreamSession>> timed_out;
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex);
                        for (auto& [handle, session] : sessions_by_handle) {
                            if (session->is_total_timeout()) {
                                timed_out.push_back(session);
                            }
                        }
                    }
                    for (auto& session : timed_out) {
                        LOG_WARN("[http][stream] total timeout exceeded, cancelling session");
                        session->cancel();
                        session->finish_with_error("Total request timeout exceeded");
                        // 从 sessions map 移除（cancel 后不再有 DONE 事件）
                        std::lock_guard<std::mutex> lock(sessions_mutex);
                        sessions_by_handle.erase(session->easy_handle());
                        // reader map 的清理交给下次 poll_loop 的 expired 回收
                    }
                }

                // C.3：用 curl_multi_wait 替代 sleep_for(10ms) 忙等
                // curl_multi_wait 内部用 select/poll，无传输时阻塞等待（最多 100ms），
                // CPU 占用近零；有数据时立即返回。libcurl 官方推荐 API。
                // 即便 still_running == 0 也用 curl_multi_wait 短轮询，避免空转
                curl_multi_wait(multi, nullptr, 0, 100, nullptr);
            } catch (const std::exception& e) {
                // 记录日志但不让异常逃逸线程函数，避免 std::terminate → abort()
                LOG_ERROR("[http] poll_loop exception: {}", e.what());
            } catch (...) {
                LOG_ERROR("[http] poll_loop unknown exception");
            }
        }
    }

    ~Impl() { shutdown(); }

    void shutdown() {
        running.store(false);
        if (poll_thread.joinable()) poll_thread.join();
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            sessions_by_handle.clear();
            sessions_by_reader.clear();
        }
        if (multi) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
    }
};

// ============================================================
// HttpClient 接口
// ============================================================

HttpClient::HttpClient() : m_impl(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() { shutdown(); }

void HttpClient::async_post_stream(const std::string& url,
                                   const std::vector<std::pair<std::string, std::string>>& headers,
                                   const std::string& body, std::shared_ptr<SSEStreamReader> reader,
                                   std::function<void()> on_complete, int timeout_ms) const {
    LOG_INFO("[http][stream] POST {} body_len={} timeout={}ms", url, body.size(), timeout_ms);
    auto parsed = parse_url(url);
    // H-4：URL 解析失败（scheme 为空）时直接 finish reader，避免构造 "://" 怪 URL
    if (parsed.scheme.empty()) {
        LOG_ERROR("[http][stream] POST {} invalid URL", url);
        reader->finish("Invalid URL: " + url);
        if (on_complete) on_complete();
        return;
    }
    auto* key = reader.get();

    const auto session = std::make_shared<StreamSession>(
        parsed, headers, body, reader, std::move(on_complete), timeout_ms, m_block_private_ips);

    if (!session->easy_handle()) {
        // curl 初始化失败：主动 finish reader 让上层能收到错误，避免 next() 无限阻塞
        // （调用方已把 reader 存入 m_active_readers，若不 finish 会永远等数据）
        // finish_with_error 会同时触发 reader->finish 和 on_complete 回调
        LOG_ERROR("[http][stream] POST {} curl_easy_init failed", url);
        session->finish_with_error("Failed to initialize curl session");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
        m_impl->sessions_by_handle[session->easy_handle()] = session;
        m_impl->sessions_by_reader[key] = session;
    }

    session->run(m_impl->multi);
}

void HttpClient::cancel_stream(SSEStreamReader* reader) {
    LOG_DEBUG("[http][stream] cancel_stream requested");
    std::shared_ptr<StreamSession> session;
    {
        std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
        auto it = m_impl->sessions_by_reader.find(reader);
        if (it != m_impl->sessions_by_reader.end()) {
            session = it->second.lock();
            if (session) {
                session->cancel();
                auto hit = m_impl->sessions_by_handle.find(session->easy_handle());
                if (hit != m_impl->sessions_by_handle.end()) m_impl->sessions_by_handle.erase(hit);
            }
            m_impl->sessions_by_reader.erase(it);
        }
    }
    // session 彻底移除后，安全触发完成回调
    if (session) session->on_transfer_done(CURLE_OK);
}

void HttpClient::shutdown() {
    if (m_impl) m_impl->shutdown();
}

}  // namespace agent