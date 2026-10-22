/**
 * @file test_sandbox.cpp
 * @brief sandbox 模块单元测试
 * @details 覆盖 SandboxConfig 工厂方法与 is_permissive()、SandboxDetector 缓存行为、
 *          SandboxAdapter wrap_command() 降级/passthrough/包装路径。
 *
 *          平台无关测试：SandboxConfig 纯数据逻辑、permissive/passthrough 路径、
 *          降级行为（Windows 无沙盒后端时必降级）。
 *          平台特定测试：macOS/Linux 的实际包装路径受编译期条件控制，
 *          在 Windows 上只验证降级路径。
 *
 * @note 测试名使用 ASCII 字符，避免 Catch2 JSON 输出在 Windows 上编码损坏
 *       导致 CMake catch_discover_tests 解析失败。
 */

#include <catch2/catch_test_macros.hpp>

#include "core/process/sandbox/sandbox_config.h"
#include "core/process/sandbox/sandbox_detector.h"
#include "core/process/sandbox/sandbox_adapter.h"

#include <filesystem>
#include <string>
#include <vector>

using namespace agent::process::sandbox;

// ============================================================
// SandboxConfig 测试
// ============================================================

// permissive 工厂方法生成无限制配置
TEST_CASE("SandboxConfig permissive factory produces unrestricted config", "[sandbox][config]") {
    auto config = SandboxConfig::permissive();
    REQUIRE(config.network_isolated == false);
    REQUIRE(config.allow_write.empty());
    REQUIRE(config.allow_read.empty());
    REQUIRE(config.deny_write.empty());
    REQUIRE(config.deny_read.empty());
    REQUIRE(config.allow_domains.empty());
    REQUIRE(config.deny_domains.empty());
}

// restrictive 工厂方法包含 cwd 读写权限
TEST_CASE("SandboxConfig restrictive factory includes cwd read/write", "[sandbox][config]") {
    std::string cwd = std::filesystem::current_path().string();
    auto config = SandboxConfig::restrictive(cwd);

    // 应包含 cwd 在 allow_write 和 allow_read 中
    REQUIRE(config.allow_write.size() >= 1);
    REQUIRE(config.allow_read.size() >= 1);

    bool found_in_write = false;
    for (const auto& p : config.allow_write) {
        if (p == cwd) {
            found_in_write = true;
            break;
        }
    }
    REQUIRE(found_in_write);

    bool found_in_read = false;
    for (const auto& p : config.allow_read) {
        if (p == cwd) {
            found_in_read = true;
            break;
        }
    }
    REQUIRE(found_in_read);

    // restrictive 默认隔离网络
    REQUIRE(config.network_isolated == true);
}

// restrictive 空 cwd 不添加路径
TEST_CASE("SandboxConfig restrictive with empty cwd adds no path", "[sandbox][config]") {
    auto config = SandboxConfig::restrictive("");
    REQUIRE(config.allow_write.empty());
    REQUIRE(config.allow_read.empty());
}

TEST_CASE("SandboxConfig is_permissive returns true for permissive", "[sandbox][config]") {
    auto config = SandboxConfig::permissive();
    REQUIRE(config.is_permissive() == true);
}

TEST_CASE("SandboxConfig is_permissive returns false for restrictive", "[sandbox][config]") {
    auto config = SandboxConfig::restrictive("/tmp");
    REQUIRE(config.is_permissive() == false);
}

TEST_CASE("SandboxConfig is_permissive returns false for network_isolated", "[sandbox][config]") {
    SandboxConfig config;
    config.network_isolated = true;
    REQUIRE(config.is_permissive() == false);
}

TEST_CASE("SandboxConfig is_permissive returns false for any deny rule", "[sandbox][config]") {
    SandboxConfig config;
    config.deny_write.push_back("/secret");
    REQUIRE(config.is_permissive() == false);
}

TEST_CASE("SandboxConfig is_permissive returns false for any allow rule", "[sandbox][config]") {
    SandboxConfig config;
    config.allow_read.push_back("/usr");
    REQUIRE(config.is_permissive() == false);
}

// ============================================================
// SandboxDetector 测试
// ============================================================

// detect 返回缓存值
TEST_CASE("SandboxDetector detect returns cached value", "[sandbox][detector]") {
    auto& detector = SandboxDetector::instance();
    detector.clear_cache();

    auto backend1 = detector.detect();
    auto backend2 = detector.detect();

    // 两次调用应返回相同结果（缓存）
    REQUIRE(backend1 == backend2);
}

// path 与 detect 一致
TEST_CASE("SandboxDetector path consistent with detect", "[sandbox][detector]") {
    auto& detector = SandboxDetector::instance();
    detector.clear_cache();

    auto backend = detector.detect();
    auto path = detector.path();

    // 若后端为 None，path 应为 nullopt
    if (backend == SandboxDetector::Backend::None) {
        REQUIRE_FALSE(path.has_value());
    } else {
        // 若后端非 None，path 应有值
        REQUIRE(path.has_value());
        REQUIRE_FALSE(path->empty());
    }
}

// is_available 与 detect 一致
TEST_CASE("SandboxDetector is_available consistent with detect", "[sandbox][detector]") {
    auto& detector = SandboxDetector::instance();
    detector.clear_cache();

    bool available = detector.is_available();
    auto backend = detector.detect();

    REQUIRE(available == (backend != SandboxDetector::Backend::None));
}

// backend_name 返回有效字符串
TEST_CASE("SandboxDetector backend_name returns valid string", "[sandbox][detector]") {
    auto& detector = SandboxDetector::instance();
    detector.clear_cache();

    auto name = detector.backend_name();
    auto backend = detector.detect();

    if (backend == SandboxDetector::Backend::Seatbelt) {
        REQUIRE(name == "seatbelt");
    } else if (backend == SandboxDetector::Backend::Bubblewrap) {
        REQUIRE(name == "bubblewrap");
    } else {
        REQUIRE(name == "none");
    }
}

// clear_cache 后可重新探测
TEST_CASE("SandboxDetector can re-detect after clear_cache", "[sandbox][detector]") {
    auto& detector = SandboxDetector::instance();
    detector.detect();  // 填充缓存
    detector.clear_cache();

    // clear_cache 后应能重新探测
    auto backend = detector.detect();
    // 重新探测结果应与平台一致（不崩溃、返回有效枚举）
    REQUIRE(backend <= SandboxDetector::Backend::Bubblewrap);
}

// ============================================================
// SandboxAdapter 测试
// ============================================================

// permissive 配置走 passthrough 路径
TEST_CASE("SandboxAdapter permissive config takes passthrough path", "[sandbox][adapter]") {
    auto config = SandboxConfig::permissive();
    auto wrapped = SandboxAdapter::wrap_command("rg", {"pattern", "."}, config);

    REQUIRE(wrapped.was_wrapped == false);
    REQUIRE(wrapped.degraded == false);
    REQUIRE(wrapped.cmd == "rg");
    REQUIRE(wrapped.args.size() == 2);
    REQUIRE(wrapped.args[0] == "pattern");
    REQUIRE(wrapped.args[1] == ".");
}

// restrictive 配置在 Windows 降级
TEST_CASE("SandboxAdapter restrictive config degrades on Windows", "[sandbox][adapter]") {
    auto config = SandboxConfig::restrictive("C:\\test");
    auto wrapped = SandboxAdapter::wrap_command("rg", {"pattern"}, config);

#ifdef _WIN32
    // Windows：无沙盒后端，应降级
    REQUIRE(wrapped.was_wrapped == false);
    REQUIRE(wrapped.degraded == true);
    REQUIRE(wrapped.cmd == "rg");
    REQUIRE(wrapped.args.size() == 1);
    REQUIRE(wrapped.args[0] == "pattern");
    REQUIRE(wrapped.backend_name == "none");
#else
    // macOS/Linux：若有沙盒后端则应包装，否则降级
    auto& detector = SandboxDetector::instance();
    detector.clear_cache();
    if (detector.is_available()) {
        REQUIRE(wrapped.was_wrapped == true);
        REQUIRE(wrapped.degraded == false);
        REQUIRE(wrapped.cmd != "rg");  // cmd 应为 sandbox-exec 或 bwrap 路径
        REQUIRE_FALSE(wrapped.backend_name.empty());
        REQUIRE(wrapped.backend_name != "none");
    } else {
        REQUIRE(wrapped.was_wrapped == false);
        REQUIRE(wrapped.degraded == true);
        REQUIRE(wrapped.cmd == "rg");
    }
#endif
}

TEST_CASE("SandboxAdapter is_enabled returns false on Windows", "[sandbox][adapter]") {
#ifdef _WIN32
    REQUIRE(SandboxAdapter::is_enabled() == false);
#else
    // macOS/Linux：取决于沙盒工具是否安装，只验证不崩溃
    bool enabled = SandboxAdapter::is_enabled();
    auto& detector = SandboxDetector::instance();
    detector.clear_cache();
    REQUIRE(enabled == detector.is_available());
#endif
}

// wrap_command 保留原命令参数
TEST_CASE("SandboxAdapter wrap_command preserves original args", "[sandbox][adapter]") {
    std::vector<std::string> args = {"--flag", "value", "file.txt"};
    auto config = SandboxConfig::permissive();
    auto wrapped = SandboxAdapter::wrap_command("git", args, config);

    REQUIRE(wrapped.cmd == "git");
    REQUIRE(wrapped.args == args);
}

// restrictive 配置带空参数
TEST_CASE("SandboxAdapter restrictive config with empty args", "[sandbox][adapter]") {
    auto config = SandboxConfig::restrictive("/tmp");
    auto wrapped = SandboxAdapter::wrap_command("rg", {}, config);

    // 不应崩溃，参数列表可为空
#ifdef _WIN32
    REQUIRE(wrapped.degraded == true);
    REQUIRE(wrapped.args.empty());
#endif
}

// 网络隔离配置不崩溃
TEST_CASE("SandboxAdapter network isolated config does not crash", "[sandbox][adapter]") {
    SandboxConfig config;
    config.network_isolated = true;
    config.allow_write.push_back("/tmp");

    auto wrapped = SandboxAdapter::wrap_command("curl", {"https://example.com"}, config);

    // 不崩溃即可；Windows 降级，macOS/Linux 视后端可用性
    REQUIRE_FALSE(wrapped.cmd.empty());
}

// allow_domains 和 deny_domains 同时配置
TEST_CASE("SandboxAdapter allow_domains and deny_domains combined", "[sandbox][adapter]") {
    SandboxConfig config;
    config.allow_domains.push_back("api.example.com");
    config.deny_domains.push_back("evil.com");
    config.allow_write.push_back("/tmp");

    auto wrapped = SandboxAdapter::wrap_command("curl", {"https://api.example.com"}, config);

    // 不崩溃即可；验证网络规则生成不会导致异常
    REQUIRE_FALSE(wrapped.cmd.empty());
}
