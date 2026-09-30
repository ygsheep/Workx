/**
 * @file test_mock_helpers.cpp
 * @brief MockConfigManager / MockEventBus / MockTaskManager 自测试
 * @details 验证三个 Mock 类的基本行为，确保它们可作为 IConfigManager / IEventBus /
 *          ITaskManager 的替代品用于隔离测试。
 */

#include "helpers/mock_config_manager.h"
#include "helpers/mock_event_bus.h"
#include "helpers/mock_task_manager.h"

#include <catch2/catch_test_macros.hpp>

#include "agent/message/types.h"  // UserInputEvent

namespace {

/// @brief 测试用事件类型（仅在本文件使用）
struct TestEvent {
    int value = 0;
};

}  // anonymous namespace

TEST_CASE("MockConfigManager basic read/write", "[mock][config]") {
    using namespace agent::test;
    MockConfigManager cfg;

    SECTION("默认无键") {
        REQUIRE_FALSE(cfg.has("missing"));
        REQUIRE(cfg.size() == 0);
    }

    SECTION("set/get_or 往返") {
        cfg.set("backend.model_name", std::string("test-model"));
        cfg.set("backend.max_tokens", 8192);
        cfg.set("backend.enable_cache", true);

        REQUIRE(cfg.has("backend.model_name"));
        REQUIRE(cfg.get_or<std::string>("backend.model_name", "") == "test-model");
        REQUIRE(cfg.get_or<int>("backend.max_tokens", 0) == 8192);
        REQUIRE(cfg.get_or<bool>("backend.enable_cache", false) == true);
        REQUIRE(cfg.size() == 3);
    }

    SECTION("get_or 缺失键返回默认值") {
        REQUIRE(cfg.get_or<std::string>("missing", "default") == "default");
        REQUIRE(cfg.get_or<int>("missing", 42) == 42);
    }

    SECTION("get 类型不匹配返回错误") {
        cfg.set("key", std::string("value"));
        auto result = cfg.get<int>("key");
        REQUIRE(result.is_err());
    }

    SECTION("load/save 默认成功") {
        REQUIRE(cfg.load_from_file("dummy.json").is_ok());
        REQUIRE(cfg.save_to_file("dummy.json").is_ok());
    }

    SECTION("load 错误注入") {
        cfg.set_load_error(std::string("file not found"));
        auto result = cfg.load_from_file("missing.json");
        REQUIRE(result.is_err());
        REQUIRE(result.error().message == "file not found");
    }

    SECTION("get_all_keys 返回所有键") {
        cfg.set("a", 1);
        cfg.set("b", 2);
        auto keys = cfg.get_all_keys();
        REQUIRE(keys.size() == 2);
    }

    SECTION("clear 清空") {
        cfg.set("a", 1);
        cfg.clear();
        REQUIRE(cfg.size() == 0);
    }
}

TEST_CASE("MockEventBus subscribe and publish records", "[mock][event_bus]") {
    using namespace agent::test;
    MockEventBus bus;

    SECTION("默认无发布") {
        REQUIRE(bus.total_published_count() == 0);
        REQUIRE(bus.unsubscribe_count() == 0);
    }

    SECTION("publish 记录类型") {
        bus.publish(TestEvent{.value = 42});
        bus.publish(TestEvent{.value = 100});
        REQUIRE(bus.published_count<TestEvent>() == 2);
        REQUIRE(bus.total_published_count() == 2);
    }

    SECTION("publish_async 入队后 process 派发") {
        bus.publish_async(TestEvent{.value = 1});
        REQUIRE(bus.async_queue_size() == 1);

        bus.process_async_events();
        REQUIRE(bus.async_queue_size() == 0);
        REQUIRE(bus.published_count<TestEvent>() == 1);
    }

    SECTION("subscribe + dispatch_enabled 触发回调") {
        bus.set_dispatch_enabled(true);
        int received = 0;
        bus.subscribe<TestEvent>([&](const TestEvent& e) { received = e.value; });
        REQUIRE(bus.subscriber_count_typed<TestEvent>() == 1);

        bus.publish(TestEvent{.value = 999});
        REQUIRE(received == 999);
    }

    SECTION("unsubscribe 减少订阅者") {
        bus.set_dispatch_enabled(true);
        auto token = bus.subscribe<TestEvent>([](const TestEvent&) {});
        REQUIRE(bus.subscriber_count_typed<TestEvent>() == 1);

        bus.unsubscribe<TestEvent>(token);
        REQUIRE(bus.subscriber_count_typed<TestEvent>() == 0);
        REQUIRE(bus.unsubscribe_count() == 1);
    }

    SECTION("clear 清空所有") {
        bus.subscribe<TestEvent>([](const TestEvent&) {});
        bus.publish_async(TestEvent{});
        bus.publish(TestEvent{});

        bus.clear();
        REQUIRE(bus.total_subscriber_count() == 0);
        REQUIRE(bus.async_queue_size() == 0);
        REQUIRE(bus.total_published_count() == 0);
    }

    SECTION("内置事件类型可用") {
        bus.publish(agent::UserInputEvent{.text = "hello"});
        REQUIRE(bus.published_count<agent::UserInputEvent>() == 1);
    }
}

TEST_CASE("MockTaskManager task creation and counting", "[mock][task_manager]") {
    using namespace agent::test;
    MockTaskManager tm;

    SECTION("默认计数为 0") {
        REQUIRE(tm.create_count() == 0);
        REQUIRE(tm.launched_count() == 0);
        REQUIRE(tm.start_count() == 0);
    }

    SECTION("create 增加计数并返回 Task") {
        auto task = tm.create("test-task", [](const std::atomic<bool>&) {});
        REQUIRE(tm.create_count() == 1);
        REQUIRE(task != nullptr);
        REQUIRE(task->getName() == "test-task");
        REQUIRE(tm.getTasks().size() == 1);
    }

    SECTION("launch 等价于 create + start") {
        auto task = tm.launch("launched-task", [](const std::atomic<bool>&) {});
        REQUIRE(tm.create_count() == 1);
        REQUIRE(tm.launched_count() == 1);
        REQUIRE(tm.start_count() == 1);
        REQUIRE(task != nullptr);
    }

    SECTION("cancel 增加计数") {
        auto task = tm.create("t", [](const std::atomic<bool>&) {});
        tm.cancel(task);
        REQUIRE(tm.cancel_count() == 1);
    }

    SECTION("update/waitForAll/cancelAll 计数") {
        tm.update();
        tm.waitForAll();
        tm.cancelAll();
        REQUIRE(tm.update_count() == 1);
        REQUIRE(tm.wait_count() == 1);
        REQUIRE(tm.cancel_all_count() == 1);
    }

    SECTION("clear_history 清空记录") {
        tm.create("t", [](const std::atomic<bool>&) {});
        tm.clear_history();
        REQUIRE(tm.create_count() == 0);
        REQUIRE(tm.getTasks().empty());
    }

    SECTION("getRunningTaskCount 默认 0") { REQUIRE(tm.getRunningTaskCount() == 0); }
}

TEST_CASE("Three Mocks can be used as interface references", "[mock][integration]") {
    using namespace agent::test;
    MockEventBus bus;
    MockConfigManager cfg;
    MockTaskManager tm;

    // 验证 Mock 可作为接口引用传入（编译期检查）
    agent::IEventBus& bus_ref = bus;
    agent::IConfigManager& cfg_ref = cfg;
    agent::ITaskManager& tm_ref = tm;

    bus_ref.publish(TestEvent{});
    cfg_ref.set_value("key", std::string("value"));
    tm_ref.update();

    REQUIRE(bus.total_published_count() == 1);
    REQUIRE(cfg.has("key"));
    REQUIRE(tm.update_count() == 1);

    (void)bus_ref;
    (void)cfg_ref;
    (void)tm_ref;
}
