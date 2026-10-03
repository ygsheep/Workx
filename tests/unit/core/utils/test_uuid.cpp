/**
 * @file test_uuid.cpp
 * @brief UUIDv4 生成工具单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/utils/uuid.h"

namespace {

/// @brief 批量采样，返回其中不重复的个数
size_t count_unique(size_t n) {
    std::unordered_set<std::string> seen;
    seen.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        seen.insert(core::util::generate_uuid());
    }
    return seen.size();
}

}  // namespace

TEST_CASE("uuid: generates valid UUIDv4 format", "[core][utils][uuid]") {
    std::string id = core::util::generate_uuid();
    // UUIDv4 格式：xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx（y 为 8/9/a/b）
    std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    REQUIRE(std::regex_match(id, re));
}

TEST_CASE("uuid: generates unique values", "[core][utils][uuid]") {
    std::string a = core::util::generate_uuid();
    std::string b = core::util::generate_uuid();
    REQUIRE(a != b);
}

TEST_CASE("uuid: empty string never returned", "[core][utils][uuid]") {
    for (int i = 0; i < 100; ++i) {
        REQUIRE_FALSE(core::util::generate_uuid().empty());
    }
}

TEST_CASE("uuid: 1000 unique checks (collision sanity)", "[core][utils][uuid]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(core::util::generate_uuid());
    }
    REQUIRE(seen.size() == 1000);
}

// ============================================================
// Issue #131：不得退回「每次调用都用单个 32 位 rd() 重新播种」
// ============================================================
// 上面那条 1000 次采样对原缺陷只有 ≈1.2e-4 的命中率（变异实测 3 次全漏），
// 它是靠 CI 跑了很久才偶然撞到一次的。样本量必须让退化实现的期望碰撞数远大于 1：
// 30 万次采样 ≈10.5，变异实测 3/3 必现（每次采样都是一次独立的 32 位播种）。
//
// ⚠️ 这条测的是**流内唯一性**，不是种子熵。thread_local 让播种每线程只发生一次，
//    之后 30 万次全来自同一条 mt19937_64 流，唯一性由 MT 的周期（2^19937-1）保证。
//    所以 seed_seq 用 8 个 rd()（256 位种子）带来的 RFC §4.4「122 位不可预测性」
//    在这里不可观测 —— 它防的是 UUID 被枚举/预测（session id 的安全含义），
//    单测统计上验证不了（需 ~2^16 次独立播种事件），只能靠注释与 review 把关。

TEST_CASE("uuid: 30 万次采样无碰撞（#131）", "[core][utils][uuid][issue131]") {
    constexpr size_t kSamples = 300000;
    REQUIRE(count_unique(kSamples) == kSamples);
}

TEST_CASE("uuid: 多线程并发生成不重复（#131：thread_local 播种）",
          "[core][utils][uuid][issue131]") {
    // 守护 #131 引入的 thread_local 本身：原实现是刻意无状态的，一旦退化成
    // 共享 static（无锁），多线程并发 mutate 同一份 MT 状态就会漏号
    // —— 变异实测得到 19955/20000，这条能抓到。
    constexpr int kThreads = 4;
    constexpr int kPerThread = 5000;
    std::mutex mu;
    std::unordered_set<std::string> seen;
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&] {
            std::vector<std::string> local;
            local.reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i) {
                local.push_back(core::util::generate_uuid());
            }
            std::lock_guard<std::mutex> lk(mu);
            for (auto& s : local) seen.insert(std::move(s));
        });
    }
    for (auto& w : workers) {
        w.join();
    }
    REQUIRE(seen.size() == static_cast<size_t>(kThreads * kPerThread));
}
