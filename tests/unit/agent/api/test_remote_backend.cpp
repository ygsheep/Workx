/**
 * @file test_remote_backend.cpp
 * @brief RemoteBackend 单元测试（H-1：DI 注入 IEventBus* 验证）
 * @details 验证 RemoteBackend 不再调用 EventBus::instance()，而是通过构造注入的
 *          IEventBus* 发布 BackendStatusEvent。使用 MockEventBus 做隔离测试。
 */

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <set>
#include <mutex>
#include <functional>

#include "agent/api/remote/remote_backend.h"
#include "agent/api/backend_factory.h"
#include "agent/api/backend_types.h"
#include "agent/message/types.h"    // BackendStatusEvent
#include "core/events/event_bus.h"  // H-A：验证未污染全局单例
#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::test;

namespace {

/// @brief 构造一个最小可用的 Remote BackendConfig
BackendConfig make_remote_config() {
    BackendConfig cfg;
    cfg.type = BackendConfig::Type::Remote;
    cfg.provider = ProviderType::OpenAI;
    cfg.base_url = "https://api.openai.com";
    cfg.api_key = "sk-test-key";
    cfg.model_name = "gpt-4o";
    cfg.timeout_ms = 5000;
    return cfg;
}

/// @brief Fake IHttpClient（M-1）：捕获 on_complete / cancel_stream 供测试驱动
/// @details 不发起真实网络请求；记录每个 reader 的 on_complete 回调，
///          测试可手动触发 complete() 模拟请求结束，验证多 reader 并发仲裁路径。
class FakeHttpClient : public IHttpClient {
   public:
    ResultV2<HttpResponse> get(const std::string&,
                               const std::vector<std::pair<std::string, std::string>>&,
                               int) override {
        return ResultV2<HttpResponse>::ok(HttpResponse{});
    }

    ResultV2<HttpResponse> post(const std::string&,
                                const std::vector<std::pair<std::string, std::string>>&,
                                const std::string&, int) override {
        return ResultV2<HttpResponse>::ok(HttpResponse{});
    }

    void async_post_stream(const std::string&,
                           const std::vector<std::pair<std::string, std::string>>&,
                           const std::string&, std::shared_ptr<SSEStreamReader> reader,
                           std::function<void()> on_complete, int) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_on_complete[reader.get()] = std::move(on_complete);
    }

    void cancel_stream(SSEStreamReader* reader) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_cancel_count;
        m_cancelled.insert(reader);
    }

    void shutdown() override {}

    /// @brief 触发指定 reader 的 on_complete（模拟请求结束）
    void complete(IStreamReader* reader) {
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_on_complete.find(reader);
            if (it == m_on_complete.end()) return;
            cb = std::move(it->second);
            m_on_complete.erase(it);
        }
        if (cb) cb();
    }

    [[nodiscard]] size_t cancel_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancel_count;
    }

    [[nodiscard]] bool was_cancelled(IStreamReader* reader) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cancelled.count(reader) > 0;
    }

   private:
    mutable std::mutex m_mutex;
    mutable std::map<IStreamReader*, std::function<void()>> m_on_complete;
    mutable std::set<IStreamReader*> m_cancelled;
    mutable size_t m_cancel_count = 0;
};

}  // namespace

// ============================================================================
// H-1：RemoteBackend 构造接收 IEventBus*，不再调用单例
// ============================================================================

TEST_CASE("RemoteBackend initialize publishes Connected via injected bus",
          "[backend][remote][h-1]") {
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("initialize 入队 Connected 事件") {
        auto result = backend.initialize(make_remote_config());
        REQUIRE(result.is_ok());
        REQUIRE(backend.is_ready());

        // publish_async 入队，未 process 前不计入 published_count
        REQUIRE(bus.async_queue_size() == 1);

        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 1);
        REQUIRE(bus.async_queue_size() == 0);
    }

    SECTION("shutdown 入队 Disconnected 事件") {
        backend.initialize(make_remote_config());
        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 1);

        backend.shutdown();
        REQUIRE(bus.async_queue_size() == 1);

        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 2);
    }

    SECTION("析构等价于 shutdown 发布 Disconnected") {
        {
            RemoteBackend b(&bus);
            b.initialize(make_remote_config());
            bus.process_async_events();
        }  // 析构调用 shutdown
        bus.process_async_events();
        // 初始化 1 次 Connected + 析构 1 次 Disconnected
        REQUIRE(bus.published_count<BackendStatusEvent>() == 2);
    }
}

TEST_CASE("RemoteBackend event content correctness", "[backend][remote][h-1]") {
    MockEventBus bus;
    bus.set_dispatch_enabled(true);

    // 订阅并捕获事件内容
    BackendStatusEvent::Status captured_status = BackendStatusEvent::Disconnected;
    std::string captured_name;
    bus.subscribe<BackendStatusEvent>([&](const BackendStatusEvent& e) {
        captured_status = e.status;
        captured_name = e.backend_name;
    });

    RemoteBackend backend(&bus);

    SECTION("initialize 发布 Connected + backend_name=remote") {
        backend.initialize(make_remote_config());
        // publish_async -> process 后同步派发到订阅者
        bus.process_async_events();

        REQUIRE(captured_status == BackendStatusEvent::Connected);
        REQUIRE(captured_name == "remote");
    }

    SECTION("shutdown 发布 Disconnected + backend_name=remote") {
        backend.initialize(make_remote_config());
        bus.process_async_events();

        backend.shutdown();
        bus.process_async_events();

        REQUIRE(captured_status == BackendStatusEvent::Disconnected);
        REQUIRE(captured_name == "remote");
    }
}

TEST_CASE("RemoteBackend nullptr event_bus skips publishing - backward compat",
          "[backend][remote][h-1]") {
    // H-1：event_bus=nullptr 时不发布，保持向后兼容
    // H-A：扩展验证未污染全局单例（Closed AI Loop 防护）
    EventBus::instance().clear();

    RemoteBackend backend(nullptr);

    SECTION("initialize 不发布事件，不污染全局单例") {
        auto result = backend.initialize(make_remote_config());
        REQUIRE(result.is_ok());
        REQUIRE(backend.is_ready());

        // H-A：验证全局单例未被污染——这是 C-1 回归的检测点
        EventBus::instance().process_async_events();
        REQUIRE(EventBus::instance().async_queue_size() == 0);
    }

    SECTION("shutdown 不发布事件，不污染全局单例") {
        backend.initialize(make_remote_config());
        backend.shutdown();
        REQUIRE_FALSE(backend.is_ready());

        EventBus::instance().process_async_events();
        REQUIRE(EventBus::instance().async_queue_size() == 0);
    }
}

TEST_CASE("RemoteBackend initialize rejects invalid config", "[backend][remote][h-1]") {
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("type 不匹配返回 InvalidInput") {
        BackendConfig cfg;
        cfg.type = BackendConfig::Type::Local;
        auto result = backend.initialize(cfg);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::InvalidInput);
        REQUIRE(bus.async_queue_size() == 0);
    }

    SECTION("base_url 为空返回 InvalidInput") {
        BackendConfig cfg = make_remote_config();
        cfg.base_url.clear();
        auto result = backend.initialize(cfg);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::InvalidInput);
        REQUIRE(bus.async_queue_size() == 0);
    }
}

TEST_CASE("BackendFactory propagates event_bus to RemoteBackend", "[backend][factory][h-1]") {
    MockEventBus bus;

    SECTION("factory create 注入 event_bus 后端发布事件") {
        auto backend = BackendFactory::create(make_remote_config(), &bus);
        REQUIRE(backend != nullptr);
        REQUIRE(backend->name() == "remote");

        backend->initialize(make_remote_config());
        REQUIRE(bus.async_queue_size() == 1);
        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 1);
    }

    SECTION("factory create 默认 nullptr 不发布，不污染全局单例") {
        // H-A：扩展验证未污染全局单例——这是 C-1 回归的核心检测点
        EventBus::instance().clear();

        auto backend = BackendFactory::create(make_remote_config(), nullptr);
        REQUIRE(backend != nullptr);
        backend->initialize(make_remote_config());
        REQUIRE(backend->is_ready());

        // 验证 BackendStatusEvent 未发布到全局单例
        EventBus::instance().process_async_events();
        REQUIRE(EventBus::instance().async_queue_size() == 0);
    }
}

// ============================================================================
// H-B / M-7：BackendState 状态机测试
// 验证单一 atomic<BackendState> 消除非法组合，状态转换符合：
//   Idle →(initialize)→ Ready →(submit)→ Generating →(完成)→ Ready →(shutdown)→ Shutdown
// Shutdown 为终态，幂等。非 Ready 态拒绝 submit_completion / list_models。
// ============================================================================

TEST_CASE("RemoteBackend state machine: initial state is Idle",
          "[backend][remote][m7][state_machine]") {
    // M-7：构造后未 initialize 前，状态为 Idle（不是 Ready / Generating / Shutdown）
    MockEventBus bus;
    RemoteBackend backend(&bus);

    REQUIRE(backend.state() == BackendState::Idle);
    REQUIRE_FALSE(backend.is_ready());
    REQUIRE_FALSE(backend.is_generating());
}

TEST_CASE("RemoteBackend state machine: Idle -> Ready via initialize",
          "[backend][remote][m7][state_machine]") {
    MockEventBus bus;
    RemoteBackend backend(&bus);

    auto result = backend.initialize(make_remote_config());
    REQUIRE(result.is_ok());

    // M-7：initialize 成功后状态转为 Ready
    REQUIRE(backend.state() == BackendState::Ready);
    REQUIRE(backend.is_ready());
    REQUIRE_FALSE(backend.is_generating());

    bus.process_async_events();
    REQUIRE(bus.published_count<BackendStatusEvent>() == 1);
}

TEST_CASE("RemoteBackend state machine: Ready -> Shutdown via shutdown",
          "[backend][remote][m7][state_machine]") {
    MockEventBus bus;
    RemoteBackend backend(&bus);
    backend.initialize(make_remote_config());
    bus.process_async_events();
    REQUIRE(backend.state() == BackendState::Ready);

    backend.shutdown();

    // M-7：shutdown 后转为 Shutdown 终态
    REQUIRE(backend.state() == BackendState::Shutdown);
    REQUIRE_FALSE(backend.is_ready());
    REQUIRE_FALSE(backend.is_generating());

    bus.process_async_events();
    // 初始化 1 次 Connected + shutdown 1 次 Disconnected
    REQUIRE(bus.published_count<BackendStatusEvent>() == 2);
}

TEST_CASE("RemoteBackend state machine: shutdown is idempotent",
          "[backend][remote][m7][state_machine]") {
    // M-7：Shutdown 是终态，重复 shutdown 应为 no-op（幂等）
    MockEventBus bus;
    RemoteBackend backend(&bus);
    backend.initialize(make_remote_config());
    bus.process_async_events();

    backend.shutdown();
    REQUIRE(backend.state() == BackendState::Shutdown);

    // 第二次 shutdown 不应崩溃，状态保持 Shutdown
    backend.shutdown();
    REQUIRE(backend.state() == BackendState::Shutdown);

    // 第三次 shutdown 仍幂等
    backend.shutdown();
    REQUIRE(backend.state() == BackendState::Shutdown);

    // 仍只发布 1 次 Disconnected（重复 shutdown 不再发布）
    bus.process_async_events();
    REQUIRE(bus.published_count<BackendStatusEvent>() == 2);
}

TEST_CASE("RemoteBackend state machine: shutdown on Idle is no-op",
          "[backend][remote][m7][state_machine]") {
    // M-7：Idle 态（未 initialize）shutdown 应为 no-op，不发布事件
    MockEventBus bus;
    RemoteBackend backend(&bus);

    REQUIRE(backend.state() == BackendState::Idle);

    backend.shutdown();

    // 仍为 Idle（shutdown 对 Idle 态不做任何操作）
    // 注：根据 shutdown() 实现，Idle 不在任何 CAS 分支中，状态保持不变
    REQUIRE(backend.state() == BackendState::Idle);
    REQUIRE(bus.async_queue_size() == 0);
}

TEST_CASE("RemoteBackend state machine: non-Ready rejects submit_completion",
          "[backend][remote][m7][state_machine]") {
    // M-7：非 Ready 态（Idle / Shutdown）拒绝 submit_completion
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("Idle 态拒绝 submit_completion") {
        REQUIRE(backend.state() == BackendState::Idle);
        CompletionRequest req;
        auto reader = backend.submit_completion(req);
        REQUIRE(reader == nullptr);
        // 状态保持 Idle
        REQUIRE(backend.state() == BackendState::Idle);
    }

    SECTION("Shutdown 态拒绝 submit_completion") {
        backend.initialize(make_remote_config());
        backend.shutdown();
        REQUIRE(backend.state() == BackendState::Shutdown);

        CompletionRequest req;
        auto reader = backend.submit_completion(req);
        REQUIRE(reader == nullptr);
        // 状态保持 Shutdown
        REQUIRE(backend.state() == BackendState::Shutdown);
    }
}

TEST_CASE("RemoteBackend state machine: non-Ready rejects list_models",
          "[backend][remote][m7][state_machine]") {
    // M-7：非 Ready 态（Idle / Shutdown）拒绝 list_models
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("Idle 态 list_models 返回 InternalError") {
        REQUIRE(backend.state() == BackendState::Idle);
        auto result = backend.list_models();
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::InternalError);
        REQUIRE(result.error().message.find("not ready") != std::string::npos);
    }

    SECTION("Shutdown 态 list_models 返回 InternalError") {
        backend.initialize(make_remote_config());
        backend.shutdown();
        REQUIRE(backend.state() == BackendState::Shutdown);

        auto result = backend.list_models();
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::InternalError);
    }
}

TEST_CASE("RemoteBackend state machine: single enum eliminates illegal combinations",
          "[backend][remote][m7][state_machine]") {
    // M-7：验证单一 atomic<BackendState> 消除非法组合
    // 原实现用两个 atomic<bool> m_ready/m_generating，可能出现：
    //   - m_ready=false 但 m_generating=true（非法）
    //   - m_ready=true 且 m_generating=true（语义冲突）
    // 现实现用单一枚举，is_ready() 和 is_generating() 互斥
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("Idle 态：is_ready=false 且 is_generating=false") {
        REQUIRE(backend.state() == BackendState::Idle);
        REQUIRE_FALSE(backend.is_ready());
        REQUIRE_FALSE(backend.is_generating());
    }

    SECTION("Ready 态：is_ready=true 且 is_generating=false") {
        backend.initialize(make_remote_config());
        REQUIRE(backend.state() == BackendState::Ready);
        REQUIRE(backend.is_ready());
        REQUIRE_FALSE(backend.is_generating());
    }

    SECTION("Shutdown 态：is_ready=false 且 is_generating=false") {
        backend.initialize(make_remote_config());
        backend.shutdown();
        REQUIRE(backend.state() == BackendState::Shutdown);
        REQUIRE_FALSE(backend.is_ready());
        REQUIRE_FALSE(backend.is_generating());
    }
}

// ============================================================================
// H-B / M-N1：on_complete CAS 契约测试
// M-N1 修复：on_complete 回调改用 CAS `Generating→Ready` 而非 store(Ready)，
// 避免边界场景下覆盖 Shutdown 终态。
// 由于 HttpClient 为具体类无法 mock，on_complete 回调和 interrupt_locked()
// 使用相同的 CAS 模式，通过 interrupt() 间接验证 CAS 在非 Generating 态下
// 不覆盖状态。
// ============================================================================

TEST_CASE("RemoteBackend interrupt on Shutdown keeps Shutdown (M-N1 CAS contract)",
          "[backend][remote][m-n1]") {
    // M-N1：验证 CAS `Generating→Ready` 在 Shutdown 态下失败，不覆盖终态
    // on_complete 回调与 interrupt_locked() 使用相同 CAS 模式：
    //   BackendState expected = Generating;
    //   m_state.compare_exchange_strong(expected, Ready, ...);
    // 当状态为 Shutdown 时 CAS 失败，保持 Shutdown 不变。
    MockEventBus bus;
    RemoteBackend backend(&bus);
    backend.initialize(make_remote_config());
    backend.shutdown();
    REQUIRE(backend.state() == BackendState::Shutdown);

    // interrupt() 内部调用 interrupt_locked()，其 CAS Generating→Ready 会失败
    backend.interrupt();

    // Shutdown 终态不被覆盖
    REQUIRE(backend.state() == BackendState::Shutdown);
    REQUIRE_FALSE(backend.is_ready());
    REQUIRE_FALSE(backend.is_generating());
}

TEST_CASE("RemoteBackend interrupt on Idle keeps Idle (M-N1 CAS contract)",
          "[backend][remote][m-n1]") {
    // M-N1：验证 CAS `Generating→Ready` 在 Idle 态下也失败，不覆盖状态
    MockEventBus bus;
    RemoteBackend backend(&bus);
    REQUIRE(backend.state() == BackendState::Idle);

    backend.interrupt();

    // Idle 态不被覆盖（CAS Generating→Ready 失败）
    REQUIRE(backend.state() == BackendState::Idle);
    REQUIRE_FALSE(backend.is_ready());
    REQUIRE_FALSE(backend.is_generating());
}

// ============================================================================
// M-1：多 reader 并发仲裁路径直接单测（注入 FakeHttpClient 驱动回调）
// 覆盖 v1.2.0 最复杂、风险最高的改动：集合增删清 + 并发状态仲裁。
// ============================================================================

TEST_CASE("RemoteBackend concurrent submits both return readers and return to Ready",
          "[backend][remote][concurrency]") {
    // M-1：两个并发 submit 均返回 reader（不再拒绝第二个），各自 on_complete 后状态回 Ready
    MockEventBus bus;
    RemoteBackend backend(&bus);
    auto fake = std::make_unique<FakeHttpClient>();
    auto* fake_ptr = fake.get();
    backend.set_http_client_for_testing(std::move(fake));
    backend.initialize(make_remote_config());
    REQUIRE(backend.is_ready());

    CompletionRequest req;
    auto r1 = backend.submit_completion(req);
    auto r2 = backend.submit_completion(req);
    REQUIRE(r1 != nullptr);
    REQUIRE(r2 != nullptr);
    REQUIRE(backend.is_generating());

    // 仅完成第一个：仍处于 Generating（第二个在飞）
    fake_ptr->complete(r1.get());
    REQUIRE(backend.is_generating());

    // 完成第二个：集合清空，状态回 Ready
    fake_ptr->complete(r2.get());
    REQUIRE(backend.is_ready());
    REQUIRE_FALSE(backend.is_generating());
}

TEST_CASE("RemoteBackend interrupt cancels all active readers", "[backend][remote][concurrency]") {
    // M-1：interrupt 遍历取消全部在飞请求并清空集合，状态回 Ready
    MockEventBus bus;
    RemoteBackend backend(&bus);
    auto fake = std::make_unique<FakeHttpClient>();
    auto* fake_ptr = fake.get();
    backend.set_http_client_for_testing(std::move(fake));
    backend.initialize(make_remote_config());

    CompletionRequest req;
    auto r1 = backend.submit_completion(req);
    auto r2 = backend.submit_completion(req);
    REQUIRE(r1 != nullptr);
    REQUIRE(r2 != nullptr);

    backend.interrupt();

    REQUIRE(fake_ptr->cancel_count() == 2);
    REQUIRE(fake_ptr->was_cancelled(r1.get()));
    REQUIRE(fake_ptr->was_cancelled(r2.get()));
    REQUIRE(backend.is_ready());

    // 中断后集合已清空，新提交可再次进入 Generating
    auto r3 = backend.submit_completion(req);
    REQUIRE(r3 != nullptr);
    REQUIRE(backend.is_generating());
    fake_ptr->complete(r3.get());
    REQUIRE(backend.is_ready());
}

TEST_CASE("RemoteBackend shutdown cancels active readers and rejects new submits",
          "[backend][remote][concurrency]") {
    // M-1：shutdown 持锁 CAS Generating→Shutdown 并清理全部 reader，Shutdown 后新请求被拒
    MockEventBus bus;
    RemoteBackend backend(&bus);
    auto fake = std::make_unique<FakeHttpClient>();
    auto* fake_ptr = fake.get();
    backend.set_http_client_for_testing(std::move(fake));
    backend.initialize(make_remote_config());

    CompletionRequest req;
    auto r1 = backend.submit_completion(req);
    auto r2 = backend.submit_completion(req);
    REQUIRE(r1 != nullptr);
    REQUIRE(r2 != nullptr);

    backend.shutdown();

    REQUIRE(backend.state() == BackendState::Shutdown);
    REQUIRE(fake_ptr->cancel_count() == 2);
    REQUIRE(fake_ptr->was_cancelled(r1.get()));
    REQUIRE(fake_ptr->was_cancelled(r2.get()));

    // Shutdown 后新请求被拒（fast-path 检查 + 锁内 CAS 双保险）
    auto r3 = backend.submit_completion(req);
    REQUIRE(r3 == nullptr);
    REQUIRE(backend.state() == BackendState::Shutdown);
}

TEST_CASE("RemoteBackend late on_complete after shutdown keeps Shutdown (M-N1)",
          "[backend][remote][concurrency]") {
    // M-1 + M-N1：shutdown 清理后，被取消请求的 on_complete 迟到触发，
    // CAS Generating→Ready 失败，不覆盖 Shutdown 终态
    MockEventBus bus;
    RemoteBackend backend(&bus);
    auto fake = std::make_unique<FakeHttpClient>();
    auto* fake_ptr = fake.get();
    backend.set_http_client_for_testing(std::move(fake));
    backend.initialize(make_remote_config());

    CompletionRequest req;
    auto r1 = backend.submit_completion(req);
    REQUIRE(r1 != nullptr);

    backend.shutdown();
    REQUIRE(backend.state() == BackendState::Shutdown);

    // 迟到 on_complete：集合已清空，CAS Generating→Ready 失败，保持 Shutdown
    fake_ptr->complete(r1.get());
    REQUIRE(backend.state() == BackendState::Shutdown);
    REQUIRE_FALSE(backend.is_ready());
}
