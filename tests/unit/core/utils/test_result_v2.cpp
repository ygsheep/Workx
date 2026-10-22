/**
 * @file test_result_v2.cpp
 * @brief ResultV2<T> 模板单元测试（V2）
 * @details 覆盖 ResultV2<T> 和 ResultV2<void> 的所有公开 API
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "core/utils/result_v2.h"
#include "core/utils/error.h"

#include <string>
#include <vector>

using namespace agent;
using namespace Catch::Matchers;

// ============================================================
// ResultV2<T> 测试
// ============================================================

TEST_CASE("ResultV2<T> ok construction", "[result_v2]") {
    SECTION("int value") {
        auto r = ResultV2<int>::ok(42);
        REQUIRE(r.is_ok());
        REQUIRE_FALSE(r.is_err());
        REQUIRE(r.value() == 42);
    }

    SECTION("string value") {
        auto r = ResultV2<std::string>::ok(std::string("hello"));
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == "hello");
    }

    SECTION("move-only type") {
        auto r = ResultV2<std::unique_ptr<int>>::ok(std::make_unique<int>(99));
        REQUIRE(r.is_ok());
        REQUIRE(*r.value() == 99);
    }
}

TEST_CASE("ResultV2<T> err construction", "[result_v2]") {
    SECTION("full Error") {
        Error err{Error::Code::NetworkTimeout, "timed out", "url=..."};
        auto r = ResultV2<int>::err(err);
        REQUIRE(r.is_err());
        REQUIRE_FALSE(r.is_ok());
        REQUIRE(r.error().code == Error::Code::NetworkTimeout);
        REQUIRE(r.error().message == "timed out");
    }

    SECTION("convenience constructor") {
        auto r = ResultV2<int>::err(Error::Code::InvalidInput, "bad value", "field=x");
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::InvalidInput);
        REQUIRE(r.error().message == "bad value");
        REQUIRE(r.error().context == "field=x");
    }

    SECTION("without context") {
        auto r = ResultV2<int>::err(Error::Code::Unknown, "failed");
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::Unknown);
        REQUIRE(r.error().message == "failed");
        REQUIRE(r.error().context.empty());
    }
}

TEST_CASE("ResultV2<T> value_or", "[result_v2]") {
    SECTION("ok returns value") {
        auto r = ResultV2<int>::ok(42);
        REQUIRE(r.value_or(0) == 42);
    }

    SECTION("err returns default") {
        auto r = ResultV2<int>::err(Error::Code::Unknown, "err");
        REQUIRE(r.value_or(99) == 99);
    }

    SECTION("string default") {
        auto r = ResultV2<std::string>::err(Error::Code::Unknown, "err");
        REQUIRE(r.value_or("default") == "default");
    }
}

TEST_CASE("ResultV2<T> map", "[result_v2]") {
    SECTION("ok maps value") {
        auto r = ResultV2<int>::ok(5).map([](int x) { return x * 2; });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == 10);
    }

    SECTION("err propagates") {
        auto r = ResultV2<int>::err(Error::Code::NetworkTimeout, "timeout").map([](int x) {
            return x * 2;
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::NetworkTimeout);
    }

    SECTION("type transformation") {
        auto r = ResultV2<int>::ok(42).map([](int x) { return std::to_string(x); });
        static_assert(std::is_same_v<decltype(r), ResultV2<std::string>>);
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == "42");
    }

    SECTION("map on move") {
        auto r = ResultV2<std::string>::ok(std::string("hello")).map([](std::string&& s) {
            return s + " world";
        });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == "hello world");
    }
}

TEST_CASE("ResultV2<T> and_then", "[result_v2]") {
    SECTION("ok chains") {
        auto r = ResultV2<int>::ok(5).and_then(
            [](int x) { return ResultV2<std::string>::ok(std::to_string(x)); });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == "5");
    }

    SECTION("err propagates") {
        auto r = ResultV2<int>::err(Error::Code::Unknown, "err").and_then([](int x) {
            return ResultV2<std::string>::ok(std::to_string(x));
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::Unknown);
    }

    SECTION("chain returns err") {
        auto r = ResultV2<int>::ok(5).and_then([](int /*x*/) {
            return ResultV2<std::string>::err(Error::Code::InvalidInput, "negative not allowed");
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::InvalidInput);
    }
}

TEST_CASE("ResultV2<T> map_err", "[result_v2]") {
    SECTION("ok unchanged") {
        auto r = ResultV2<int>::ok(42).map_err([](Error e) {
            e.message = "modified";
            return e;
        });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == 42);
    }

    SECTION("err mapped") {
        auto r = ResultV2<int>::err(Error::Code::NetworkTimeout, "timeout").map_err([](Error e) {
            e.code = Error::Code::InternalError;
            return e;
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::InternalError);
    }
}

// ============================================================
// ResultV2<void> 特化测试
// ============================================================

TEST_CASE("ResultV2<void> ok", "[result_v2]") {
    auto r = ResultV2<void>::ok();
    REQUIRE(r.is_ok());
    REQUIRE_FALSE(r.is_err());
}

TEST_CASE("ResultV2<void> err", "[result_v2]") {
    SECTION("full Error") {
        auto r = ResultV2<void>::err(Error{Error::Code::Unknown, "failed"});
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::Unknown);
        REQUIRE(r.error().message == "failed");
    }

    SECTION("convenience constructor") {
        auto r = ResultV2<void>::err(Error::Code::ConfigInvalid, "bad config", "key=timeout");
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::ConfigInvalid);
        REQUIRE(r.error().context == "key=timeout");
    }
}

TEST_CASE("ResultV2<void> map", "[result_v2]") {
    SECTION("ok maps to value") {
        bool called = false;
        auto r = ResultV2<void>::ok().map([&]() {
            called = true;
            return 42;
        });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == 42);
        REQUIRE(called);
    }

    SECTION("err propagates") {
        auto r = ResultV2<void>::err(Error::Code::Unknown, "err").map([]() { return 42; });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::Unknown);
    }
}

TEST_CASE("ResultV2<void> and_then", "[result_v2]") {
    SECTION("ok chains") {
        auto r = ResultV2<void>::ok().and_then([]() { return ResultV2<int>::ok(99); });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == 99);
    }

    SECTION("err propagates") {
        auto r = ResultV2<void>::err(Error::Code::Unknown, "err").and_then([]() {
            return ResultV2<int>::ok(99);
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::Unknown);
    }
}

TEST_CASE("ResultV2<void> map_err", "[result_v2]") {
    SECTION("ok unchanged") {
        auto r = ResultV2<void>::ok().map_err([](Error e) {
            e.code = Error::Code::InternalError;
            return e;
        });
        REQUIRE(r.is_ok());
    }

    SECTION("err mapped") {
        auto r = ResultV2<void>::err(Error::Code::NetworkTimeout, "timeout").map_err([](Error e) {
            e.code = Error::Code::InternalError;
            return e;
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::InternalError);
    }
}

// ============================================================
// TRY_RESULT_V2 宏测试
// ============================================================

namespace {

ResultV2<int> try_return_ok() { return ResultV2<int>::ok(10); }

ResultV2<int> try_return_err() {
    return ResultV2<int>::err(Error::Code::NetworkTimeout, "timed out", "test");
}

ResultV2<int> try_test_helper(bool call_err) {
    TRY_RESULT_V2(val, call_err ? try_return_err() : try_return_ok());
    return ResultV2<int>::ok(val.value() + 5);
}

}  // anonymous namespace

TEST_CASE("TRY_RESULT_V2 macro", "[result_v2]") {
    SECTION("ok path") {
        auto r = try_test_helper(false);
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == 15);  // 10 + 5
    }

    SECTION("err propagates") {
        auto r = try_test_helper(true);
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::NetworkTimeout);
        REQUIRE(r.error().message == "timed out");
    }
}

// ============================================================
// 链式调用组合测试
// ============================================================

TEST_CASE("ResultV2 chained operations", "[result_v2]") {
    // 模拟：解析字符串 → 转换为 int → 加倍 → 转字符串
    auto parse = [](const std::string& s) -> ResultV2<int> {
        try {
            return ResultV2<int>::ok(std::stoi(s));
        } catch (...) {
            return ResultV2<int>::err(Error::Code::InvalidInput, "parse failed", s);
        }
    };

    SECTION("all ok") {
        auto r = parse("21").map([](int x) { return x * 2; }).and_then([](int x) {
            return ResultV2<std::string>::ok(std::to_string(x));
        });
        REQUIRE(r.is_ok());
        REQUIRE(r.value() == "42");
    }

    SECTION("parse fails") {
        auto r = parse("abc").map([](int x) { return x * 2; }).and_then([](int x) {
            return ResultV2<std::string>::ok(std::to_string(x));
        });
        REQUIRE(r.is_err());
        REQUIRE(r.error().code == Error::Code::InvalidInput);
        REQUIRE(r.error().context == "abc");
    }
}
