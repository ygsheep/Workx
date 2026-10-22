/**
 * @file test_file_edit_tool.cpp
 * @brief FileEditTool + path_matcher + secret_scanner + line_endings 单元测试
 * @details 覆盖 validate_input P0 错误码（0-10）+ call 基础流程 + 辅助模块。
 *          测试使用临时目录隔离文件系统副作用，每个 TEST_CASE 后清理状态。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>

#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileReadState/file_read_state.h"
#include "agent/tool/path_matcher.h"
#include "agent/tool/secret_scanner.h"
#include "agent/tool/line_endings.h"
#include "agent/tool/quote_normalizer.h"
#include "agent/tool/encoding.h"
#include "agent/tool/file_history.h"
#include "agent/tool/context.h"
#include "core/config/config_manager.h"
#include "agent/config/app_config.h"

namespace fs = std::filesystem;
using namespace agent;
using namespace agent::tool;

// ============================================================
// 测试辅助
// ============================================================

namespace {

/// @brief 临时目录 RAII：构造时创建，析构时清理
class TempDir {
   public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(100000, 999999);
        path_ = fs::temp_directory_path() / ("workx_test_" + std::to_string(dist(gen)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

    /// 创建文件（含内容）
    fs::path make_file(const std::string& name, const std::string& content) const {
        auto fp = path_ / name;
        if (fp.has_parent_path()) {
            fs::create_directories(fp.parent_path());
        }
        std::ofstream out(fp, std::ios::binary);
        out << content;
        out.close();
        return fp;
    }

   private:
    fs::path path_;
};

/// @brief 测试环境 RAII：清理 ConfigManager + FileReadStateTracker + FileHistory
class TestEnv {
   public:
    TestEnv() {
        ConfigManager::instance()
            .clear();  // L-5：原 clear_for_test() 已移除，改用语义相同的 clear()
        FileReadStateTracker::instance().clear_for_test();
        FileHistory::instance().clear_for_test();
    }
    ~TestEnv() {
        ConfigManager::instance()
            .clear();  // L-5：原 clear_for_test() 已移除，改用语义相同的 clear()
        FileReadStateTracker::instance().clear_for_test();
        FileHistory::instance().clear_for_test();
    }

    /// @brief 为 ToolContext 注入 IConfigManager（H-5：强制 DI）
    /// @details ToolContext 含 std::atomic 不可拷贝/移动，故采用填充引用方式。
    ///          所有需要调用 validate_input/call 的测试用例都应使用此方法，
    ///          避免 ToolContext::config_manager() 因 nullptr 抛 std::logic_error。
    static ToolContext& setup_ctx(ToolContext& ctx) {
        ctx.config_manager_ptr = &ConfigManager::instance();
        return ctx;
    }
};

/// @brief 构造 Edit 工具输入 JSON
nlohmann::json make_edit_input(const std::string& file_path, const std::string& old_string,
                               const std::string& new_string,
                               std::optional<bool> replace_all = std::nullopt) {
    nlohmann::json j = {
        {"file_path", file_path}, {"old_string", old_string}, {"new_string", new_string}};
    if (replace_all.has_value()) {
        j["replace_all"] = *replace_all;
    }
    return j;
}

/// @brief 记录文件已读状态（绕过 Read 工具，直接操作 FileReadStateTracker）
void record_file_read(const fs::path& path, const std::string& content) {
    std::error_code ec;
    auto mtime = fs::last_write_time(path, ec);
    FileReadStateTracker::instance().record_read(path.generic_string(), content,
                                                 ec ? std::filesystem::file_time_type{} : mtime,
                                                 false  // 完整视图
    );
}

}  // anonymous namespace

// ============================================================
// path_matcher 测试
// ============================================================

TEST_CASE("path_matcher basic glob", "[path_matcher]") {
    SECTION("literal match") {
        REQUIRE(match_path_glob("/foo/bar", "/foo/bar"));
        REQUIRE_FALSE(match_path_glob("/foo/bar", "/foo/baz"));
    }
    SECTION("single * within segment") {
        REQUIRE(match_path_glob("/foo/bar", "/foo/*"));
        REQUIRE(match_path_glob("/foo/baz", "/foo/*"));
        REQUIRE_FALSE(match_path_glob("/foo/sub/bar", "/foo/*"));
    }
    SECTION("** cross segments") {
        REQUIRE(match_path_glob("/a/b/c/d", "/a/**/d"));
        REQUIRE(match_path_glob("/a/d", "/a/**/d"));
        REQUIRE(match_path_glob("/a/b/c", "/**/c"));
    }
    SECTION("? single char") {
        REQUIRE(match_path_glob("/foo/bar", "/foo/ba?"));
        REQUIRE_FALSE(match_path_glob("/foo/bar", "/foo/b?"));
    }
    SECTION("suffix ** matches all") {
        REQUIRE(match_path_glob("/home/user/.ssh/key", "/home/user/.ssh/**"));
        REQUIRE(match_path_glob("/home/user/.ssh/sub/deep/key", "/home/user/.ssh/**"));
    }
}

TEST_CASE("path_matcher matches_any_pattern", "[path_matcher]") {
    std::vector<std::string> patterns = {"/home/user/.ssh/**", "**/.env", "**/.git/**"};
    SECTION("hits first pattern") {
        REQUIRE(matches_any_pattern("/home/user/.ssh/id_rsa", patterns));
    }
    SECTION("hits basename pattern") {
        REQUIRE(matches_any_pattern("/any/path/.env", patterns));
        REQUIRE(matches_any_pattern("/root/project/.env", patterns));
    }
    SECTION("hits directory pattern") {
        REQUIRE(matches_any_pattern("/project/.git/config", patterns));
        REQUIRE(matches_any_pattern("/a/b/.git/refs/heads", patterns));
    }
    SECTION("no match") {
        REQUIRE_FALSE(matches_any_pattern("/home/user/code/main.cpp", patterns));
        REQUIRE_FALSE(matches_any_pattern("/etc/passwd", patterns));
    }
}

TEST_CASE("path_matcher to_posix_path", "[path_matcher]") {
    REQUIRE(to_posix_path("C:\\Users\\foo") == "C:/Users/foo");
    REQUIRE(to_posix_path("/home/foo") == "/home/foo");
    REQUIRE(to_posix_path("a/b/c") == "a/b/c");
}

// ============================================================
// secret_scanner 测试
// ============================================================

TEST_CASE("secret_scanner detects common secrets", "[secret_scanner]") {
    SECTION("AWS access token") {
        auto m = scan_for_secrets("aws_key = AKIAIOSFODNN7EXAMPLE");
        REQUIRE(!m.empty());
        REQUIRE(m[0].rule_id == "aws-access-token");
    }
    SECTION("GitHub PAT") {
        auto m = scan_for_secrets("token = ghp_0123456789012345678901234567890123456");
        REQUIRE(!m.empty());
        REQUIRE(m[0].rule_id == "github-pat");
    }
    SECTION("Anthropic API key") {
        auto m = scan_for_secrets("key = sk-ant-api03-0123456789abcdef0123456789abcdef01234567");
        REQUIRE(!m.empty());
        REQUIRE(m[0].rule_id == "anthropic-api-key");
    }
    SECTION("OpenAI API key") {
        auto m = scan_for_secrets("key = sk-proj-0123456789abcdef01234567");
        REQUIRE(!m.empty());
        REQUIRE(m[0].rule_id == "openai-api-key");
    }
    SECTION("Private key block") {
        auto m = scan_for_secrets(
            "-----BEGIN RSA PRIVATE KEY-----\nMIIEpAIBAAKCAQEA\n-----END RSA PRIVATE KEY-----");
        REQUIRE(!m.empty());
        REQUIRE(m[0].rule_id == "private-key");
    }
    SECTION("Slack bot token") {
        auto m = scan_for_secrets("token = xoxb-1234567890-1234567890-abcdefghij12");
        REQUIRE(!m.empty());
        REQUIRE(m[0].rule_id == "slack-bot-token");
    }
}

TEST_CASE("secret_scanner false negatives", "[secret_scanner]") {
    SECTION("normal text no match") {
        REQUIRE(scan_for_secrets("hello world").empty());
        REQUIRE(scan_for_secrets("int x = 42;").empty());
    }
    SECTION("short strings not matched") {
        // AKIA + 仅 15 字符（不足 16）
        REQUIRE(scan_for_secrets("AKIA01234567890123").empty());
    }
}

TEST_CASE("secret_scanner dedup by rule_id", "[secret_scanner]") {
    // 同一规则多次命中只返回一次
    std::string content =
        "key1 = ghp_0123456789012345678901234567890123456 "
        "key2 = ghp_9876543210987654321098765432109876543210";
    auto m = scan_for_secrets(content);
    REQUIRE(m.size() == 1);
    REQUIRE(m[0].rule_id == "github-pat");
}

TEST_CASE("secret_scanner error message", "[secret_scanner]") {
    SECTION("no secret returns empty") { REQUIRE(scan_for_secret_error("normal text").empty()); }
    SECTION("with secret returns formatted message") {
        std::string msg = scan_for_secret_error("key = ghp_0123456789012345678901234567890123456");
        REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("potential secrets"));
        REQUIRE_THAT(msg, Catch::Matchers::ContainsSubstring("GitHub PAT"));
    }
}

// ============================================================
// FileEditTool::validate_input 测试
// ============================================================

TEST_CASE("FileEditTool validate_input missing fields", "[file_edit_tool]") {
    TestEnv env;
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);

    SECTION("missing file_path") {
        auto input = nlohmann::json{{"old_string", "a"}, {"new_string", "b"}};
        auto r = tool.validate_input(input, ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("file_path"));
    }
    SECTION("missing old_string") {
        auto input = nlohmann::json{{"file_path", "/tmp/foo"}, {"new_string", "b"}};
        auto r = tool.validate_input(input, ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("old_string"));
    }
    SECTION("missing new_string") {
        auto input = nlohmann::json{{"file_path", "/tmp/foo"}, {"old_string", "a"}};
        auto r = tool.validate_input(input, ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("new_string"));
    }
    SECTION("empty file_path") {
        auto r = tool.validate_input(make_edit_input("", "a", "b"), ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("empty"));
    }
}

TEST_CASE("FileEditTool validate_input error code 1 - old==new", "[file_edit_tool]") {
    TestEnv env;
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    // H-1: 使用 fs::temp_directory_path() 构造跨平台绝对路径，避免 POSIX 风格在 Windows
    // 上被误判为相对路径
    std::string abs_path = (fs::temp_directory_path() / "workx_test_dummy").string();
    auto r = tool.validate_input(make_edit_input(abs_path, "abc", "abc"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("same"));
}

TEST_CASE("FileEditTool validate_input error code 4 - file not exist", "[file_edit_tool]") {
    TestEnv env;
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    // H-1: 跨平台绝对路径
    std::string abs_path = (fs::temp_directory_path() / "nonexistent_workx_test_path").string();
    auto r = tool.validate_input(make_edit_input(abs_path, "old", "new"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("does not exist"));
}

TEST_CASE("FileEditTool validate_input error code 5 - .ipynb", "[file_edit_tool]") {
    TestEnv env;
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    // H-1: 跨平台绝对路径
    std::string abs_path = (fs::temp_directory_path() / "test.ipynb").string();
    auto r = tool.validate_input(make_edit_input(abs_path, "", "new content"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("Jupyter"));
}

// ============================================================
// M-2: issue #13 路径展开测试
// @details 原 issue #13 要求强制绝对路径校验，后改为 expand_path() 自动展开
//          相对路径（基于 ctx.cwd），对齐 Claude Code CLI 行为。
//          现在相对路径不再被拒绝，而是展开后走后续存在性校验。
// ============================================================

TEST_CASE("FileEditTool validate_input expands relative path", "[file_edit_tool][issue-13]") {
    TestEnv env;
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);

    SECTION("simple relative path is expanded and checked for existence") {
        auto r = tool.validate_input(make_edit_input("foo.txt", "a", "b"), ctx);
        REQUIRE(r.is_err());
        // 相对路径已展开，文件不存在时返回 "does not exist" 而非 "absolute path"
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("does not exist"));
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("foo.txt"));
    }
    SECTION("subdir relative path is expanded and checked for existence") {
        auto r = tool.validate_input(make_edit_input("subdir/foo.txt", "a", "b"), ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("does not exist"));
    }
    SECTION("POSIX-style /tmp path is expanded on all platforms") {
        // /tmp/foo 在 POSIX 上是绝对路径，在 Windows 上由 expand_path 展开
        // 两种情况下文件都不存在，都应返回 "does not exist"
        auto r = tool.validate_input(make_edit_input("/tmp/foo", "a", "b"), ctx);
        REQUIRE(r.is_err());
        REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("does not exist"));
    }
}

TEST_CASE("FileEditTool validate_input accepts absolute path", "[file_edit_tool][issue-13]") {
    TestEnv env;
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    // 跨平台绝对路径：不会触发 "absolute path" 错误，而是走到后续校验
    std::string abs_path = (fs::temp_directory_path() / "workx_abs_test.txt").string();
    auto r = tool.validate_input(make_edit_input(abs_path, "a", "b"), ctx);
    REQUIRE(r.is_err());
    // 绝对路径通过了 is_relative() 校验，错误应该是 "does not exist" 而非 "absolute path"
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("does not exist"));
    REQUIRE_FALSE(r.error().message.find("absolute path") != std::string::npos);
}

TEST_CASE("FileEditTool validate_input error code 3 - file exists non-empty + old=empty",
          "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("existing.txt", "existing content");
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "", "new content"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("already exists"));
}

TEST_CASE("FileEditTool validate_input error code 6 - not pre-read", "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("unread.txt", "line1\nline2\n");
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "line1", "replaced"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("not been read"));
}

TEST_CASE("FileEditTool validate_input error code 7 - staleness", "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("stale.txt", "old content\n");
    // 记录一个旧 mtime（比当前早）
    auto current_mtime = fs::last_write_time(fp);
    auto older_mtime = current_mtime - std::chrono::hours(1);
    FileReadStateTracker::instance().record_read(fp.generic_string(),
                                                 "different content\n",  // 内容不同，触发 staleness
                                                 older_mtime, false);
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "old content", "new"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("modified since read"));
}

TEST_CASE("FileEditTool validate_input error code 8 - no match", "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("nomatch.txt", "hello world\n");
    record_file_read(fp, "hello world");
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r =
        tool.validate_input(make_edit_input(fp.string(), "nonexistent string", "replacement"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("not found"));
}

TEST_CASE("FileEditTool validate_input error code 9 - multiple matches, no replace_all",
          "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("multi.txt", "foo\nfoo\nfoo\n");
    record_file_read(fp, "foo\nfoo\nfoo");
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "foo", "bar"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("matches"));
}

TEST_CASE("FileEditTool validate_input error code 9 bypassed with replace_all",
          "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("multi.txt", "foo\nfoo\nfoo\n");
    record_file_read(fp, "foo\nfoo\nfoo");
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "foo", "bar", true), ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool validate_input happy path single match", "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("ok.txt", "first\nsecond\nthird\n");
    record_file_read(fp, "first\nsecond\nthird");
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "second", "SECOND"), ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool validate_input create new file - old=empty, file not exist",
          "[file_edit_tool]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.path() / "newfile.txt";
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "", "new content"), ctx);
    REQUIRE(r.is_ok());
}

// ============================================================
// FileEditTool validate_input 错误码 0/2 (新实现)
// ============================================================

TEST_CASE("FileEditTool validate_input error code 0 - secret scanning",
          "[file_edit_tool][secret]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("with_secret.txt", "placeholder\n");
    record_file_read(fp, "placeholder");

    // 启用 secret 扫描
    ConfigManager::instance().set(keys::EDIT_SCAN_SECRETS, true);

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r =
        tool.validate_input(make_edit_input(fp.string(), "placeholder",
                                            "token = ghp_0123456789012345678901234567890123456"),
                            ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("secrets"));
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("GitHub PAT"));
}

TEST_CASE("FileEditTool validate_input secret scan disabled by default",
          "[file_edit_tool][secret]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("with_secret.txt", "placeholder\n");
    record_file_read(fp, "placeholder");

    // 不开启扫描，默认放行（虽然内容含密钥，但 validate_input 不会拒绝）
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r =
        tool.validate_input(make_edit_input(fp.string(), "placeholder",
                                            "token = ghp_0123456789012345678901234567890123456"),
                            ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool validate_input error code 2 - deny rules", "[file_edit_tool][deny]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("denied.env", "KEY=value\n");
    record_file_read(fp, "KEY=value");

    // 配置 deny 规则：所有 .env 文件
    ConfigManager::instance().set(keys::EDIT_DENY_PATTERNS, std::string("**/.env"));

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "KEY=value", "KEY=other"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("denied"));
}

TEST_CASE("FileEditTool validate_input deny rules not matching", "[file_edit_tool][deny]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("normal.txt", "hello\n");
    record_file_read(fp, "hello");

    ConfigManager::instance().set(keys::EDIT_DENY_PATTERNS, std::string("**/.env\n**/.ssh/**"));

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "hello", "world"), ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool validate_input deny rules empty config", "[file_edit_tool][deny]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("ok.txt", "hello\n");
    record_file_read(fp, "hello");

    // 空配置：deny 检查跳过
    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    auto r = tool.validate_input(make_edit_input(fp.string(), "hello", "world"), ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool validate_input deny rules comments and whitespace",
          "[file_edit_tool][deny]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("normal.txt", "hello\n");
    record_file_read(fp, "hello");

    // 配置含注释行和空白行，应被正确跳过
    ConfigManager::instance().set(keys::EDIT_DENY_PATTERNS,
                                  std::string("# comment line\n\n  \n  **/.env  \n"));

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    // normal.txt 不匹配 **/.env，应通过
    auto r = tool.validate_input(make_edit_input(fp.string(), "hello", "world"), ctx);
    REQUIRE(r.is_ok());
}

// ============================================================
// FileEditTool::call 测试
// ============================================================

TEST_CASE("FileEditTool call create new file", "[file_edit_tool][call]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.path() / "created.txt";

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "", "new content\nline 2"), ctx);
    REQUIRE(r.is_ok());
    REQUIRE(fs::exists(fp));
    REQUIRE_THAT(r.value().to_string(), Catch::Matchers::ContainsSubstring("created successfully"));

    // 验证文件内容
    std::ifstream in(fp);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content == "new content\nline 2");

    // 验证状态已记录（写后可立即编辑）
    auto state = FileReadStateTracker::instance().get_state(fp.generic_string());
    REQUIRE(state.has_value());
}

TEST_CASE("FileEditTool call update existing file", "[file_edit_tool][call]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("update.txt", "old line\nkeep line\n");
    record_file_read(fp, "old line\nkeep line");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "old line", "NEW LINE"), ctx);
    REQUIRE(r.is_ok());

    // 验证文件内容
    std::ifstream in(fp);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content == "NEW LINE\nkeep line\n");

    // 验证 .bak 备份已生成
    REQUIRE(fs::exists(fs::path(fp.string() + ".bak")));
}

TEST_CASE("FileEditTool call replace_all", "[file_edit_tool][call]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("multi.txt", "foo\nfoo\nfoo\n");
    record_file_read(fp, "foo\nfoo\nfoo");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "foo", "bar", true), ctx);
    REQUIRE(r.is_ok());

    std::ifstream in(fp);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content == "bar\nbar\nbar\n");
}

TEST_CASE("FileEditTool call rejects when not pre-read", "[file_edit_tool][call]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("unread.txt", "content\n");
    // 不调用 record_file_read，模拟未读取

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "content", "new"), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("not been read"));
}

TEST_CASE("FileEditTool update cancelled keeps original file intact",
          "[file_edit_tool][call][cancel]") {
    // #23 P2：打断发生在备份完成后、写文件前（取消点），
    // 原文件完整、.bak 保留、无残留临时文件。
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("cancel_update.txt", "old line\nkeep line\n");
    record_file_read(fp, "old line\nkeep line");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();
    ctx.cancel();  // 置位内部取消标志

    auto r = tool.call(make_edit_input(fp.string(), "old line", "NEW LINE"), ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::Cancelled);

    // 原文件内容不变
    std::ifstream in(fp);
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    REQUIRE(content == "old line\nkeep line\n");

    // 无残留临时文件（原子写：临时文件 + rename，取消时已清理）
    REQUIRE_FALSE(fs::exists(fs::path(fp.string() + ".workx.tmp")));
}

TEST_CASE("FileEditTool create cancelled leaves no file", "[file_edit_tool][call][cancel]") {
    // #23 P2：新建模式取消点，原文件（不存在）依旧不存在、无残留临时文件。
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.path() / "cancel_create.txt";

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();
    ctx.cancel();

    auto r = tool.call(make_edit_input(fp.string(), "", "content\n"), ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::Cancelled);

    REQUIRE_FALSE(fs::exists(fp));
    REQUIRE_FALSE(fs::exists(fs::path(fp.string() + ".workx.tmp")));
}

// ============================================================
// line_endings 单元测试
// ============================================================

TEST_CASE("line_endings detect_line_ending", "[line_endings]") {
    SECTION("pure LF") {
        REQUIRE(detect_line_ending("a\nb\nc\n") == LineEnding::LF);
        REQUIRE(detect_line_ending("a\nb") == LineEnding::LF);
    }
    SECTION("pure CRLF") {
        REQUIRE(detect_line_ending("a\r\nb\r\nc\r\n") == LineEnding::CRLF);
        REQUIRE(detect_line_ending("a\r\nb") == LineEnding::CRLF);
    }
    SECTION("pure CR") {
        REQUIRE(detect_line_ending("a\rb\rc\r") == LineEnding::CR);
        REQUIRE(detect_line_ending("a\rb") == LineEnding::CR);
    }
    SECTION("mixed CRLF + LF (CRLF majority)") {
        REQUIRE(detect_line_ending("a\r\nb\r\nc\nd\n") == LineEnding::CRLF);
    }
    SECTION("mixed LF + CRLF (LF majority)") {
        REQUIRE(detect_line_ending("a\nb\nc\r\nd\n") == LineEnding::LF);
    }
    SECTION("tie CRLF and LF → CRLF wins") {
        REQUIRE(detect_line_ending("a\r\nb\n") == LineEnding::CRLF);
    }
    SECTION("empty content defaults to LF") { REQUIRE(detect_line_ending("") == LineEnding::LF); }
    SECTION("no line breaks defaults to LF") {
        REQUIRE(detect_line_ending("single line no break") == LineEnding::LF);
    }
}

TEST_CASE("line_endings apply_line_ending", "[line_endings]") {
    SECTION("LF passthrough") {
        REQUIRE(apply_line_ending("a\nb\nc\n", LineEnding::LF) == "a\nb\nc\n");
        REQUIRE(apply_line_ending("", LineEnding::LF) == "");
    }
    SECTION("LF to CRLF") {
        REQUIRE(apply_line_ending("a\nb\nc\n", LineEnding::CRLF) == "a\r\nb\r\nc\r\n");
        REQUIRE(apply_line_ending("no newline", LineEnding::CRLF) == "no newline");
    }
    SECTION("LF to CR") {
        REQUIRE(apply_line_ending("a\nb\nc\n", LineEnding::CR) == "a\rb\rc\r");
        REQUIRE(apply_line_ending("no newline", LineEnding::CR) == "no newline");
    }
}

TEST_CASE("line_endings normalize_to_lf", "[line_endings]") {
    SECTION("CRLF to LF") { REQUIRE(normalize_to_lf("a\r\nb\r\nc\r\n") == "a\nb\nc\n"); }
    SECTION("isolated CR to LF") { REQUIRE(normalize_to_lf("a\rb\rc\r") == "a\nb\nc\n"); }
    SECTION("mixed to LF") { REQUIRE(normalize_to_lf("a\r\nb\rc\nd") == "a\nb\nc\nd"); }
    SECTION("already LF") { REQUIRE(normalize_to_lf("a\nb\n") == "a\nb\n"); }
    SECTION("preserves trailing newline") {
        REQUIRE(normalize_to_lf("foo\r\n") == "foo\n");
        REQUIRE(normalize_to_lf("foo\n") == "foo\n");
        REQUIRE(normalize_to_lf("foo") == "foo");
    }
}

TEST_CASE("line_endings round-trip", "[line_endings]") {
    // 原始 → LF → 原始 应保持一致（对于纯一种行尾的文件）
    SECTION("CRLF round-trip") {
        std::string original = "line1\r\nline2\r\nline3\r\n";
        std::string lf = normalize_to_lf(original);
        std::string restored = apply_line_ending(lf, detect_line_ending(original));
        REQUIRE(restored == original);
    }
    SECTION("CR round-trip") {
        std::string original = "line1\rline2\rline3\r";
        std::string lf = normalize_to_lf(original);
        std::string restored = apply_line_ending(lf, detect_line_ending(original));
        REQUIRE(restored == original);
    }
    SECTION("LF round-trip") {
        std::string original = "line1\nline2\nline3\n";
        std::string lf = normalize_to_lf(original);
        std::string restored = apply_line_ending(lf, detect_line_ending(original));
        REQUIRE(restored == original);
    }
}

TEST_CASE("line_endings line_ending_name", "[line_endings]") {
    REQUIRE(std::string(line_ending_name(LineEnding::LF)) == "LF");
    REQUIRE(std::string(line_ending_name(LineEnding::CRLF)) == "CRLF");
    REQUIRE(std::string(line_ending_name(LineEnding::CR)) == "CR");
}

// ============================================================
// FileEditTool 行尾保留集成测试
// ============================================================

/// 读取文件原始字节流（binary 模式）
static std::string read_file_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST_CASE("FileEditTool preserves CRLF line endings", "[file_edit_tool][line_endings]") {
    TestEnv env;
    TempDir tmp;
    // 原文件使用 CRLF 行尾
    auto fp = tmp.make_file("crlf.txt", "old line\r\nkeep line\r\n");
    record_file_read(fp, "old line\nkeep line");  // state 存 LF 规范化版本

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "old line", "NEW LINE"), ctx);
    REQUIRE(r.is_ok());

    // 验证写回的文件仍为 CRLF
    std::string content = read_file_bytes(fp);
    REQUIRE(content == "NEW LINE\r\nkeep line\r\n");
    REQUIRE(content.find("\r\n") != std::string::npos);
    REQUIRE(content.find("\n") != std::string::npos);
    // 不应含孤立 LF（所有 \n 前必有 \r）
    size_t pos = 0;
    while ((pos = content.find('\n', pos)) != std::string::npos) {
        REQUIRE(pos > 0);
        REQUIRE(content[pos - 1] == '\r');
        ++pos;
    }
}

TEST_CASE("FileEditTool preserves LF line endings", "[file_edit_tool][line_endings]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("lf.txt", "old line\nkeep line\n");
    record_file_read(fp, "old line\nkeep line");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "old line", "NEW LINE"), ctx);
    REQUIRE(r.is_ok());

    std::string content = read_file_bytes(fp);
    REQUIRE(content == "NEW LINE\nkeep line\n");
    // 不应含 \r
    REQUIRE(content.find('\r') == std::string::npos);
}

TEST_CASE("FileEditTool preserves CR line endings", "[file_edit_tool][line_endings]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("cr.txt", "old line\rkeep line\r");
    record_file_read(fp, "old line\nkeep line");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "old line", "NEW LINE"), ctx);
    REQUIRE(r.is_ok());

    std::string content = read_file_bytes(fp);
    REQUIRE(content == "NEW LINE\rkeep line\r");
    // 不应含 \n（所有换行都是孤立 \r）
    REQUIRE(content.find('\n') == std::string::npos);
}

TEST_CASE("FileEditTool replace_all preserves CRLF", "[file_edit_tool][line_endings]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("multi_crlf.txt", "foo\r\nfoo\r\nfoo\r\n");
    record_file_read(fp, "foo\nfoo\nfoo");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "foo", "bar", true), ctx);
    REQUIRE(r.is_ok());

    std::string content = read_file_bytes(fp);
    REQUIRE(content == "bar\r\nbar\r\nbar\r\n");
}

TEST_CASE("FileEditTool no newline file preserves no-newline", "[file_edit_tool][line_endings]") {
    TestEnv env;
    TempDir tmp;
    // 文件不含任何换行符
    auto fp = tmp.make_file("none.txt", "only one line no newline");
    record_file_read(fp, "only one line no newline");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "one line", "two lines"), ctx);
    REQUIRE(r.is_ok());

    std::string content = read_file_bytes(fp);
    REQUIRE(content == "only two lines no newline");
    REQUIRE(content.find('\n') == std::string::npos);
    REQUIRE(content.find('\r') == std::string::npos);
}

// ============================================================
// quote_normalizer 单元测试
// ============================================================

// 弯引号 UTF-8 字节常量
static const std::string kSmartSingleLeft = "\xE2\x80\x98";   // '
static const std::string kSmartSingleRight = "\xE2\x80\x99";  // '
static const std::string kSmartDoubleLeft = "\xE2\x80\x9C";   // "
static const std::string kSmartDoubleRight = "\xE2\x80\x9D";  // "

TEST_CASE("quote_normalizer normalize_quotes", "[quote_normalizer]") {
    SECTION("smart single quotes to straight") {
        REQUIRE(normalize_quotes(kSmartSingleLeft + "hello" + kSmartSingleRight) == "'hello'");
    }
    SECTION("smart double quotes to straight") {
        REQUIRE(normalize_quotes(kSmartDoubleLeft + "hello" + kSmartDoubleRight) == "\"hello\"");
    }
    SECTION("mixed smart quotes") {
        std::string input = kSmartDoubleLeft + "It's" + kSmartDoubleRight;
        REQUIRE(normalize_quotes(input) == "\"It's\"");
    }
    SECTION("no quotes unchanged") { REQUIRE(normalize_quotes("hello world") == "hello world"); }
    SECTION("straight quotes unchanged") {
        REQUIRE(normalize_quotes("\"straight\" and 'single'") == "\"straight\" and 'single'");
    }
    SECTION("em dash NOT normalized") {
        // U+2014 = E2 80 94, should NOT be transformed
        std::string em_dash = "\xE2\x80\x94";
        REQUIRE(normalize_quotes("a" + em_dash + "b") == "a" + em_dash + "b");
    }
    SECTION("en dash NOT normalized") {
        // U+2013 = E2 80 93, should NOT be transformed
        std::string en_dash = "\xE2\x80\x93";
        REQUIRE(normalize_quotes("a" + en_dash + "b") == "a" + en_dash + "b");
    }
}

TEST_CASE("quote_normalizer find_actual_string exact match", "[quote_normalizer]") {
    std::string file = "hello \"world\" foo";
    SECTION("exact match returns search string") {
        auto r = find_actual_string(file, "\"world\"");
        REQUIRE(r.has_value());
        REQUIRE(*r == "\"world\"");
    }
    SECTION("no match returns nullopt") {
        auto r = find_actual_string(file, "\"nonexistent\"");
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("quote_normalizer find_actual_string smart quote match", "[quote_normalizer]") {
    // 文件使用弯引号，LLM 提供直引号
    std::string file = "hello " + kSmartDoubleLeft + "world" + kSmartDoubleRight + " foo";

    SECTION("straight quote search matches smart quote in file") {
        auto r = find_actual_string(file, "\"world\"");
        REQUIRE(r.has_value());
        // 返回的是文件中的实际子串（含弯引号）
        REQUIRE(*r == kSmartDoubleLeft + "world" + kSmartDoubleRight);
    }

    SECTION("returns nullopt when truly not found") {
        auto r = find_actual_string(file, "\"nonexistent\"");
        REQUIRE_FALSE(r.has_value());
    }
}

TEST_CASE("quote_normalizer find_actual_string single quotes", "[quote_normalizer]") {
    std::string file =
        "it" + kSmartSingleRight + "s a " + kSmartSingleLeft + "test" + kSmartSingleRight;
    SECTION("straight single matches smart single") {
        auto r = find_actual_string(file, "'test'");
        REQUIRE(r.has_value());
        REQUIRE(*r == kSmartSingleLeft + "test" + kSmartSingleRight);
    }
}

TEST_CASE("quote_normalizer count_actual_occurrences", "[quote_normalizer]") {
    SECTION("exact match count") {
        std::string file = "foo bar foo bar foo";
        REQUIRE(count_actual_occurrences(file, "foo") == 3);
    }
    SECTION("smart quote match count") {
        std::string file = kSmartDoubleLeft + "a" + kSmartDoubleRight + " " + kSmartDoubleLeft +
                           "a" + kSmartDoubleRight;
        // 文件含 2 个 "a"（弯引号），LLM 搜索 "a"（直引号）
        REQUIRE(count_actual_occurrences(file, "\"a\"") == 2);
    }
    SECTION("no match count is 0") {
        REQUIRE(count_actual_occurrences("hello world", "nonexistent") == 0);
    }
}

TEST_CASE("quote_normalizer preserve_quote_style no normalization", "[quote_normalizer]") {
    SECTION("old == actual_old returns new unchanged") {
        // 精确匹配，未发生引号规范化
        std::string old_s = "\"hello\"";
        std::string actual_old = "\"hello\"";
        std::string new_s = "\"world\"";
        REQUIRE(preserve_quote_style(old_s, actual_old, new_s) == "\"world\"");
    }
    SECTION("actual_old has no smart quotes returns new unchanged") {
        // 即使 old != actual_old，但 actual_old 无弯引号，原样返回
        std::string old_s = "abc";
        std::string actual_old = "abc";  // same → no normalization
        std::string new_s = "\"xyz\"";
        REQUIRE(preserve_quote_style(old_s, actual_old, new_s) == "\"xyz\"");
    }
}

TEST_CASE("quote_normalizer preserve_quote_style double quotes", "[quote_normalizer]") {
    // 文件使用弯双引号，LLM 提供直引号
    std::string old_s = "\"hello\"";
    std::string actual_old = kSmartDoubleLeft + "hello" + kSmartDoubleRight;
    std::string new_s = "\"world\"";

    std::string result = preserve_quote_style(old_s, actual_old, new_s);
    // new_string 的直引号应被转换为弯引号
    REQUIRE(result == kSmartDoubleLeft + "world" + kSmartDoubleRight);
}

TEST_CASE("quote_normalizer preserve_quote_style single quotes", "[quote_normalizer]") {
    SECTION("opening and closing single quotes") {
        std::string old_s = "'hello'";
        std::string actual_old = kSmartSingleLeft + "hello" + kSmartSingleRight;
        std::string new_s = "'world'";

        std::string result = preserve_quote_style(old_s, actual_old, new_s);
        REQUIRE(result == kSmartSingleLeft + "world" + kSmartSingleRight);
    }
    SECTION("apostrophe in contraction") {
        // don't 中的 ' 是缩写形式，应转换为右单引号 U+2019
        std::string old_s = "dont";
        std::string actual_old = "dont";  // same, no normalization
        std::string new_s = "don't";

        // actual_old == old_s → no normalization → 原样返回
        std::string result = preserve_quote_style(old_s, actual_old, new_s);
        REQUIRE(result == "don't");
    }
    SECTION("apostrophe with smart quote context") {
        // 文件使用弯引号，new_string 含缩写形式
        std::string old_s = "'test'";
        std::string actual_old = kSmartSingleLeft + "test" + kSmartSingleRight;
        std::string new_s = "don't go";

        std::string result = preserve_quote_style(old_s, actual_old, new_s);
        // 缩写 ' 应转换为 U+2019（右单引号）
        // "go" 前的空格后的 ' 不存在，只有 don't 中的 '
        REQUIRE(result == "don" + kSmartSingleRight + "t go");
    }
}

TEST_CASE("quote_normalizer preserve_quote_style mixed", "[quote_normalizer]") {
    // 文件同时使用弯单引号和弯双引号
    std::string old_s = "\"hello's\"";
    std::string actual_old =
        kSmartDoubleLeft + "hello" + kSmartSingleRight + "s" + kSmartDoubleRight;
    std::string new_s = "\"world's\"";

    std::string result = preserve_quote_style(old_s, actual_old, new_s);
    REQUIRE(result == kSmartDoubleLeft + "world" + kSmartSingleRight + "s" + kSmartDoubleRight);
}

// ============================================================
// FileEditTool 引号规范化集成测试
// ============================================================

TEST_CASE("FileEditTool smart quote matching", "[file_edit_tool][quote_normalizer]") {
    TestEnv env;
    TempDir tmp;
    // 文件使用弯引号
    auto fp = tmp.make_file("smart.txt",
                            "hello " + kSmartDoubleLeft + "world" + kSmartDoubleRight + "!\n");
    record_file_read(fp, "hello " + kSmartDoubleLeft + "world" + kSmartDoubleRight + "!");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // LLM 提供直引号，应能匹配文件中的弯引号
    auto r = tool.call(make_edit_input(fp.string(), "\"world\"", "\"C++\""), ctx);
    REQUIRE(r.is_ok());

    // 写回的文件应保留弯引号风格
    std::string content = read_file_bytes(fp);
    REQUIRE(content == "hello " + kSmartDoubleLeft + "C++" + kSmartDoubleRight + "!\n");
}

TEST_CASE("FileEditTool exact quote match - no normalization",
          "[file_edit_tool][quote_normalizer]") {
    TestEnv env;
    TempDir tmp;
    // 文件使用直引号
    auto fp = tmp.make_file("straight.txt", "hello \"world\"!\n");
    record_file_read(fp, "hello \"world\"!");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // LLM 提供直引号，精确匹配
    auto r = tool.call(make_edit_input(fp.string(), "\"world\"", "\"C++\""), ctx);
    REQUIRE(r.is_ok());

    // 写回的文件应使用直引号（无规范化发生）
    std::string content = read_file_bytes(fp);
    REQUIRE(content == "hello \"C++\"!\n");
}

TEST_CASE("FileEditTool smart quote matching validate_input",
          "[file_edit_tool][quote_normalizer]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file(
        "smart_val.txt", "function " + kSmartDoubleLeft + "test" + kSmartDoubleRight + "() {}\n");
    record_file_read(fp, "function " + kSmartDoubleLeft + "test" + kSmartDoubleRight + "() {}");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);

    // LLM 提供直引号，validate_input 应通过（引号规范化匹配成功）
    auto r = tool.validate_input(make_edit_input(fp.string(), "\"test\"", "\"prod\""), ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool smart quote no match returns error", "[file_edit_tool][quote_normalizer]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("nomatch_smart.txt",
                            "hello " + kSmartDoubleLeft + "world" + kSmartDoubleRight + "!\n");
    record_file_read(fp, "hello " + kSmartDoubleLeft + "world" + kSmartDoubleRight + "!");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);

    // LLM 提供的字符串在文件中不存在（即使引号规范化后也不匹配）
    auto r = tool.validate_input(make_edit_input(fp.string(), "\"nonexistent\"", "\"x\""), ctx);
    REQUIRE(r.is_err());
    REQUIRE_THAT(r.error().message, Catch::Matchers::ContainsSubstring("not found"));
}

// ============================================================
// encoding 测试（read_file_as_utf8 + write_file_with_encoding）
// ============================================================

/// @brief 构造 UTF-16LE 字节流（含 BOM）
static std::string make_utf16le(const std::string& utf8) {
    std::string out;
    out.push_back(static_cast<char>(0xFF));  // BOM
    out.push_back(static_cast<char>(0xFE));
    // 简单 ASCII 范围转换（测试用，每个 ASCII 字符 = 1 个 UTF-16LE 码元）
    for (unsigned char c : utf8) {
        out.push_back(static_cast<char>(c));
        out.push_back(static_cast<char>(0x00));
    }
    return out;
}

/// @brief 构造 UTF-16BE 字节流（含 BOM）
static std::string make_utf16be(const std::string& utf8) {
    std::string out;
    out.push_back(static_cast<char>(0xFE));  // BOM
    out.push_back(static_cast<char>(0xFF));
    for (unsigned char c : utf8) {
        out.push_back(static_cast<char>(0x00));
        out.push_back(static_cast<char>(c));
    }
    return out;
}

TEST_CASE("encoding detect BOM", "[encoding]") {
    TempDir tmp;
    SECTION("UTF-8 BOM") {
        std::string content = "\xEF\xBB\xBFhello world";
        auto fp = tmp.make_file("utf8bom.txt", content);
        REQUIRE(detect_encoding(fp) == Encoding::Utf8);
    }
    SECTION("UTF-16LE BOM") {
        auto fp = tmp.make_file("utf16le.txt", make_utf16le("hello"));
        REQUIRE(detect_encoding(fp) == Encoding::Utf16LE);
    }
    SECTION("UTF-16BE BOM") {
        auto fp = tmp.make_file("utf16be.txt", make_utf16be("hello"));
        REQUIRE(detect_encoding(fp) == Encoding::Utf16BE);
    }
    SECTION("plain UTF-8 no BOM") {
        // 含多字节 UTF-8 字符（非纯 ASCII）
        auto fp = tmp.make_file("utf8.txt", std::string("plain \xE4\xBD\xA0\xE5\xA5\xBD"));
        REQUIRE(detect_encoding(fp) == Encoding::Utf8);
    }
    SECTION("ASCII") {
        auto fp = tmp.make_file("ascii.txt", "ascii only");
        REQUIRE(detect_encoding(fp) == Encoding::Ascii);
    }
}

TEST_CASE("encoding read_file_as_utf8", "[encoding]") {
    TempDir tmp;
    SECTION("UTF-8 with BOM skips BOM") {
        std::string content = std::string("\xEF\xBB\xBF") + "hello world";
        auto fp = tmp.make_file("utf8bom.txt", content);
        std::string utf8 = read_file_as_utf8(fp, Encoding::Utf8);
        REQUIRE(utf8 == "hello world");
    }
    SECTION("UTF-16LE decoded to UTF-8") {
        auto fp = tmp.make_file("utf16le.txt", make_utf16le("hello\nworld"));
        std::string utf8 = read_file_as_utf8(fp, Encoding::Utf16LE);
        REQUIRE(utf8 == "hello\nworld");
    }
    SECTION("UTF-16BE decoded to UTF-8") {
        auto fp = tmp.make_file("utf16be.txt", make_utf16be("hello\nworld"));
        std::string utf8 = read_file_as_utf8(fp, Encoding::Utf16BE);
        REQUIRE(utf8 == "hello\nworld");
    }
    SECTION("UTF-8 multibyte content") {
        // 中文 UTF-8 字节流
        std::string content = "\xE4\xBD\xA0\xE5\xA5\xBD";  // "你好"
        auto fp = tmp.make_file("utf8cn.txt", content);
        std::string utf8 = read_file_as_utf8(fp, Encoding::Utf8);
        REQUIRE(utf8 == content);
    }
}

TEST_CASE("encoding write_file_with_encoding round-trip", "[encoding]") {
    TempDir tmp;
    SECTION("UTF-16LE round-trip") {
        std::string original = "hello\nworld\n";
        auto fp = tmp.path() / "rt_utf16le.txt";
        REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf16LE));

        // 验证文件以 BOM 开头
        std::string raw = read_file_bytes(fp);
        REQUIRE(raw.size() >= 2);
        REQUIRE(static_cast<unsigned char>(raw[0]) == 0xFF);
        REQUIRE(static_cast<unsigned char>(raw[1]) == 0xFE);

        // 读回应等于原文
        std::string read_back = read_file_as_utf8(fp, Encoding::Utf16LE);
        REQUIRE(read_back == original);
    }
    SECTION("UTF-16BE round-trip") {
        std::string original = "foo bar baz\n";
        auto fp = tmp.path() / "rt_utf16be.txt";
        REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf16BE));

        std::string raw = read_file_bytes(fp);
        REQUIRE(raw.size() >= 2);
        REQUIRE(static_cast<unsigned char>(raw[0]) == 0xFE);
        REQUIRE(static_cast<unsigned char>(raw[1]) == 0xFF);

        std::string read_back = read_file_as_utf8(fp, Encoding::Utf16BE);
        REQUIRE(read_back == original);
    }
    SECTION("UTF-8 no BOM on write") {
        std::string original = "plain content\n";
        auto fp = tmp.path() / "rt_utf8.txt";
        REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf8));

        // UTF-8 不应添加 BOM
        std::string raw = read_file_bytes(fp);
        REQUIRE(raw == original);
    }
    SECTION("UTF-8 multibyte round-trip") {
        std::string original = "\xE4\xBD\xA0\xE5\xA5\xBD\n";  // "你好\n"
        auto fp = tmp.path() / "rt_utf8cn.txt";
        REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf8));

        std::string read_back = read_file_as_utf8(fp, Encoding::Utf8);
        REQUIRE(read_back == original);
    }
    SECTION("UTF-16LE with multibyte UTF-8 content") {
        // 中文 + ASCII 混合，写入 UTF-16LE 后读回
        std::string original = std::string("hello \xE4\xBD\xA0\xE5\xA5\xBD\n");
        auto fp = tmp.path() / "rt_utf16le_cn.txt";
        REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf16LE));

        // 文件应以 BOM 开头
        std::string raw = read_file_bytes(fp);
        REQUIRE(static_cast<unsigned char>(raw[0]) == 0xFF);
        REQUIRE(static_cast<unsigned char>(raw[1]) == 0xFE);

        std::string read_back = read_file_as_utf8(fp, Encoding::Utf16LE);
        REQUIRE(read_back == original);
    }
}

TEST_CASE("encoding write_file_with_encoding cancel leaves original intact", "[encoding][cancel]") {
    // #23 P2：取消回调返回 true 时（rename 前），函数返回 false、
    // 原文件不变、无残留临时文件。
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("enc_cancel.txt", "original\n");
    std::ifstream before(fp);
    std::string original((std::istreambuf_iterator<char>(before)),
                         std::istreambuf_iterator<char>());
    REQUIRE(original == "original\n");

    auto always_cancel = []() { return true; };
    REQUIRE_FALSE(write_file_with_encoding(fp, "replaced\n", Encoding::Utf8, always_cancel));

    // 原文件未被覆盖
    std::ifstream after(fp);
    std::string after_content((std::istreambuf_iterator<char>(after)),
                              std::istreambuf_iterator<char>());
    REQUIRE(after_content == "original\n");

    // 无残留临时文件
    REQUIRE_FALSE(fs::exists(fs::path(fp.string() + ".workx.tmp")));
}

TEST_CASE("encoding encoding_name", "[encoding]") {
    REQUIRE(std::string(encoding_name(Encoding::Utf8)) == "UTF-8");
    REQUIRE(std::string(encoding_name(Encoding::Utf16LE)) == "UTF-16LE");
    REQUIRE(std::string(encoding_name(Encoding::Utf16BE)) == "UTF-16BE");
    REQUIRE(std::string(encoding_name(Encoding::Gbk)) == "GBK");
    REQUIRE(std::string(encoding_name(Encoding::Ascii)) == "ASCII");
    REQUIRE(std::string(encoding_name(Encoding::Binary)) == "Binary");
    REQUIRE(std::string(encoding_name(Encoding::Unknown)) == "Unknown");
}

// ============================================================
// FileEditTool 编码保留集成测试
// ============================================================

TEST_CASE("FileEditTool preserves UTF-16LE encoding", "[file_edit_tool][encoding]") {
    TestEnv env;
    TempDir tmp;
    // 写入 UTF-16LE 文件
    auto fp = tmp.path() / "utf16le_edit.txt";
    std::string original_utf8 = "hello world\nfoo bar\n";
    REQUIRE(write_file_with_encoding(fp, original_utf8, Encoding::Utf16LE));

    // FileReadStateTracker 存储的是 UTF-8 内容
    record_file_read(fp, "hello world\nfoo bar");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // 编辑文件
    auto r = tool.call(make_edit_input(fp.string(), "world", "C++"), ctx);
    REQUIRE(r.is_ok());

    // 验证文件仍是 UTF-16LE 编码（BOM 完整）
    std::string raw = read_file_bytes(fp);
    REQUIRE(raw.size() >= 2);
    REQUIRE(static_cast<unsigned char>(raw[0]) == 0xFF);
    REQUIRE(static_cast<unsigned char>(raw[1]) == 0xFE);

    // 验证内容已正确替换
    std::string read_back = read_file_as_utf8(fp, Encoding::Utf16LE);
    REQUIRE(read_back == "hello C++\nfoo bar\n");
}

TEST_CASE("FileEditTool preserves UTF-16BE encoding", "[file_edit_tool][encoding]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.path() / "utf16be_edit.txt";
    std::string original_utf8 = "function test() {}\n";
    REQUIRE(write_file_with_encoding(fp, original_utf8, Encoding::Utf16BE));

    record_file_read(fp, "function test() {}");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "test", "prod"), ctx);
    REQUIRE(r.is_ok());

    // 验证仍为 UTF-16BE
    std::string raw = read_file_bytes(fp);
    REQUIRE(raw.size() >= 2);
    REQUIRE(static_cast<unsigned char>(raw[0]) == 0xFE);
    REQUIRE(static_cast<unsigned char>(raw[1]) == 0xFF);

    std::string read_back = read_file_as_utf8(fp, Encoding::Utf16BE);
    REQUIRE(read_back == "function prod() {}\n");
}

TEST_CASE("FileEditTool UTF-16LE with multibyte content", "[file_edit_tool][encoding]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.path() / "utf16le_cn_edit.txt";
    // UTF-8 字节流：中文 + ASCII 混合
    std::string original = std::string("hello \xE4\xBD\xA0\xE5\xA5\xBD\n");  // "hello 你好\n"
    REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf16LE));

    // FileReadStateTracker 存储的是 UTF-8 内容
    record_file_read(fp, "hello \xE4\xBD\xA0\xE5\xA5\xBD");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // 替换 "hello" 为 "hi"
    auto r = tool.call(make_edit_input(fp.string(), "hello", "hi"), ctx);
    REQUIRE(r.is_ok());

    // 验证仍为 UTF-16LE
    std::string raw = read_file_bytes(fp);
    REQUIRE(static_cast<unsigned char>(raw[0]) == 0xFF);
    REQUIRE(static_cast<unsigned char>(raw[1]) == 0xFE);

    // 验证内容（UTF-8 字节流：hi + 空格 + 你好 + \n）
    std::string expected = std::string("hi \xE4\xBD\xA0\xE5\xA5\xBD\n");
    std::string read_back = read_file_as_utf8(fp, Encoding::Utf16LE);
    REQUIRE(read_back == expected);
}

TEST_CASE("FileEditTool UTF-16LE validate_input matching", "[file_edit_tool][encoding]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.path() / "utf16le_val.txt";
    std::string original = "hello world\nfoo bar\n";
    REQUIRE(write_file_with_encoding(fp, original, Encoding::Utf16LE));

    record_file_read(fp, "hello world\nfoo bar");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);

    // validate_input 应能正确匹配 UTF-16LE 文件中的子串
    auto r = tool.validate_input(make_edit_input(fp.string(), "world", "C++"), ctx);
    REQUIRE(r.is_ok());
}

TEST_CASE("FileEditTool UTF-8 with BOM preserves no BOM on write", "[file_edit_tool][encoding]") {
    TestEnv env;
    TempDir tmp;
    // UTF-8 with BOM
    auto fp = tmp.make_file("utf8bom_edit.txt", std::string("\xEF\xBB\xBF") + "hello world\n");
    record_file_read(fp, "hello world");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    auto r = tool.call(make_edit_input(fp.string(), "world", "C++"), ctx);
    REQUIRE(r.is_ok());

    // 写回后不应有 BOM（对齐 CC 行为：UTF-8 不保留 BOM）
    std::string raw = read_file_bytes(fp);
    REQUIRE(raw.size() >= 3);
    bool has_bom = (static_cast<unsigned char>(raw[0]) == 0xEF) &&
                   (static_cast<unsigned char>(raw[1]) == 0xBB) &&
                   (static_cast<unsigned char>(raw[2]) == 0xBF);
    REQUIRE_FALSE(has_bom);
    REQUIRE(raw == "hello C++\n");
}

// ============================================================
// file_history 测试
// ============================================================

TEST_CASE("file_history save and retrieve version", "[file_history]") {
    TestEnv env;
    const std::string path = "/test/file.txt";

    SECTION("save_version returns version_id and content is preserved") {
        std::string vid = FileHistory::instance().save_version(path, "hello\nworld\n");
        REQUIRE_FALSE(vid.empty());

        auto versions = FileHistory::instance().get_versions(path);
        REQUIRE(versions.size() == 1);
        REQUIRE(versions[0].version_id == vid);
        REQUIRE(versions[0].content == "hello\nworld\n");
        REQUIRE(versions[0].operation == "before_edit");
    }

    SECTION("multiple saves preserve order (newest first)") {
        FileHistory::instance().save_version(path, "v1\n");
        FileHistory::instance().save_version(path, "v2\n");
        FileHistory::instance().save_version(path, "v3\n");

        auto versions = FileHistory::instance().get_versions(path);
        REQUIRE(versions.size() == 3);
        REQUIRE(versions[0].content == "v3\n");
        REQUIRE(versions[1].content == "v2\n");
        REQUIRE(versions[2].content == "v1\n");
    }
}

TEST_CASE("file_history get_version by id", "[file_history]") {
    TestEnv env;
    const std::string path = "/test/get_by_id.txt";

    std::string vid1 = FileHistory::instance().save_version(path, "first\n");
    std::string vid2 = FileHistory::instance().save_version(path, "second\n");

    SECTION("get existing version by id") {
        auto v = FileHistory::instance().get_version(path, vid1);
        REQUIRE(v.has_value());
        REQUIRE(v->content == "first\n");
        REQUIRE(v->version_id == vid1);
    }

    SECTION("get non-existent version returns nullopt") {
        auto v = FileHistory::instance().get_version(path, "nonexistent_id");
        REQUIRE_FALSE(v.has_value());
    }
}

TEST_CASE("file_history get_latest_version", "[file_history]") {
    TestEnv env;
    const std::string path = "/test/latest.txt";

    SECTION("returns nullopt when no versions") {
        auto v = FileHistory::instance().get_latest_version(path);
        REQUIRE_FALSE(v.has_value());
    }

    SECTION("returns the most recent version") {
        FileHistory::instance().save_version(path, "old\n");
        FileHistory::instance().save_version(path, "new\n");

        auto v = FileHistory::instance().get_latest_version(path);
        REQUIRE(v.has_value());
        REQUIRE(v->content == "new\n");
    }
}

TEST_CASE("file_history clear_versions", "[file_history]") {
    TestEnv env;
    const std::string path = "/test/clear.txt";

    FileHistory::instance().save_version(path, "v1\n");
    FileHistory::instance().save_version(path, "v2\n");
    REQUIRE(FileHistory::instance().get_versions(path).size() == 2);

    FileHistory::instance().clear_versions(path);
    REQUIRE(FileHistory::instance().get_versions(path).empty());
    REQUIRE_FALSE(FileHistory::instance().get_latest_version(path).has_value());
}

TEST_CASE("file_history auto-pruning", "[file_history]") {
    TestEnv env;
    const std::string path = "/test/prune.txt";
    FileHistory::instance().set_max_versions(3);

    // 保存 5 个版本
    for (int i = 1; i <= 5; ++i) {
        FileHistory::instance().save_version(path, "v" + std::to_string(i) + "\n");
    }

    auto versions = FileHistory::instance().get_versions(path);
    REQUIRE(versions.size() == 3);
    // 最新的 3 个版本保留（v5, v4, v3）
    REQUIRE(versions[0].content == "v5\n");
    REQUIRE(versions[1].content == "v4\n");
    REQUIRE(versions[2].content == "v3\n");
}

TEST_CASE("file_history different files are independent", "[file_history]") {
    TestEnv env;
    const std::string path1 = "/test/file1.txt";
    const std::string path2 = "/test/file2.txt";

    FileHistory::instance().save_version(path1, "f1-v1\n");
    FileHistory::instance().save_version(path2, "f2-v1\n");
    FileHistory::instance().save_version(path1, "f1-v2\n");

    REQUIRE(FileHistory::instance().get_versions(path1).size() == 2);
    REQUIRE(FileHistory::instance().get_versions(path2).size() == 1);

    auto latest1 = FileHistory::instance().get_latest_version(path1);
    REQUIRE(latest1->content == "f1-v2\n");

    auto latest2 = FileHistory::instance().get_latest_version(path2);
    REQUIRE(latest2->content == "f2-v1\n");
}

// ============================================================
// FileEditTool + file_history 集成测试
// ============================================================

TEST_CASE("FileEditTool saves version to history before edit", "[file_edit_tool][file_history]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("history_test.txt", "hello world\nfoo bar\n");
    record_file_read(fp, "hello world\nfoo bar");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // 执行编辑
    auto r = tool.call(make_edit_input(fp.string(), "world", "C++"), ctx);
    REQUIRE(r.is_ok());

    // FileHistory 应有 1 个版本（编辑前的原始内容）
    auto versions = FileHistory::instance().get_versions(fp.generic_string());
    REQUIRE(versions.size() == 1);
    REQUIRE(versions[0].content == "hello world\nfoo bar\n");
    REQUIRE(versions[0].operation == "before_edit");
}

TEST_CASE("FileEditTool multiple edits create multiple versions",
          "[file_edit_tool][file_history]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("multi_history.txt", "alpha\nbeta\ngamma\n");
    record_file_read(fp, "alpha\nbeta\ngamma");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // 第一次编辑
    auto r1 = tool.call(make_edit_input(fp.string(), "alpha", "ALPHA"), ctx);
    REQUIRE(r1.is_ok());

    // 更新 FileReadStateTracker 以反映新内容
    record_file_read(fp, "ALPHA\nbeta\ngamma");

    // 第二次编辑
    auto r2 = tool.call(make_edit_input(fp.string(), "beta", "BETA"), ctx);
    REQUIRE(r2.is_ok());

    // FileHistory 应有 2 个版本
    auto versions = FileHistory::instance().get_versions(fp.generic_string());
    REQUIRE(versions.size() == 2);

    // 最新版本在前（第二次编辑前的内容）
    REQUIRE(versions[0].content == "ALPHA\nbeta\ngamma\n");
    // 旧版本在后（第一次编辑前的内容）
    REQUIRE(versions[1].content == "alpha\nbeta\ngamma\n");
}

TEST_CASE("FileEditTool history enables undo to previous version",
          "[file_edit_tool][file_history]") {
    TestEnv env;
    TempDir tmp;
    auto fp = tmp.make_file("undo_test.txt", "original content\n");
    record_file_read(fp, "original content");

    FileEditTool tool;
    ToolContext ctx;
    TestEnv::setup_ctx(ctx);
    ctx.cwd = tmp.path().string();

    // 编辑文件
    auto r = tool.call(make_edit_input(fp.string(), "original", "modified"), ctx);
    REQUIRE(r.is_ok());
    REQUIRE(read_file_bytes(fp) == "modified content\n");

    // 模拟 undo：从 FileHistory 取回旧版本并恢复
    auto latest = FileHistory::instance().get_latest_version(fp.generic_string());
    REQUIRE(latest.has_value());
    REQUIRE(latest->content == "original content\n");

    // 写回旧版本
    std::ofstream out(fp, std::ios::binary | std::ios::trunc);
    out << latest->content;
    out.close();

    // 验证 undo 成功
    REQUIRE(read_file_bytes(fp) == "original content\n");
}
