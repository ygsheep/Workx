/**
 * @file test_git_checkpoint.cpp
 * @brief Issue #81：git 检查点单元测试
 * @details 用临时 git 仓库覆盖 capture → diff → rollback 全链路；
 *          回滚只针对自建临时文件，不触碰开发工作区。
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "agent/util/git_checkpoint.h"
#include "core/process/subprocess.h"

namespace fs = std::filesystem;

using agent::util::GitCheckpoint;

namespace {

/// @brief 在临时目录执行 git 命令，返回是否成功
bool git_run(const fs::path& cwd, const std::vector<std::string>& args) {
    auto res = agent::process::exec("git", agent::process::ExecOptions{
                                               .cwd = cwd.string(),
                                               .args = args,
                                               .timeout = std::chrono::milliseconds(5000),
                                           });
    return res.is_ok() && res.value().exit_code == 0;
}

/// @brief 写文本文件（覆盖）
void write_file(const fs::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

/// @brief 读文本文件并去除 CR
/// @details Windows 下 git checkout（core.autocrlf）与 ofstream 文本模式都会写 CRLF，
///          断言内容时统一归一化为 LF，避免平台差异。
std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

/// @brief 临时 git 仓库夹具：init + 提交一个初始文件，析构时删除整个目录
struct TempRepo {
    fs::path dir;
    bool ready = false;

    TempRepo() {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        dir = fs::temp_directory_path() / ("workx_cp_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (ec) return;

        // -c user.* 内联配置，避免依赖宿主 git 全局配置（CI 常未设置）
        const std::vector<std::string> identity = {"-c", "user.email=t@t", "-c", "user.name=t"};
        write_file(dir / "a.txt", "line1\n");
        auto add_args = identity;
        add_args.insert(add_args.end(), {"add", "a.txt"});
        auto commit_args = identity;
        commit_args.insert(commit_args.end(), {"commit", "-q", "-m", "init"});
        ready = git_run(dir, {"init", "-q"}) && git_run(dir, add_args) && git_run(dir, commit_args);
    }

    ~TempRepo() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

}  // namespace

TEST_CASE("GitCheckpoint format_summary renders empty and non-empty results", "[git_checkpoint]") {
    agent::util::GitDiffSummary empty;
    empty.base_commit = "abc1234";
    REQUIRE(GitCheckpoint::format_summary(empty).find("无文件改动") != std::string::npos);

    agent::util::GitDiffSummary s;
    s.base_commit = "abc1234";
    s.clean_at_capture = true;
    s.files.push_back({"src/a.cpp", "M", 10, 2});
    s.total_insertions = 10;
    s.total_deletions = 2;
    const std::string text = GitCheckpoint::format_summary(s);
    REQUIRE(text.find("src/a.cpp") != std::string::npos);
    REQUIRE(text.find("+10") != std::string::npos);
}

TEST_CASE("GitCheckpoint short_sha truncates only full hashes", "[git_checkpoint]") {
    REQUIRE(GitCheckpoint::short_sha("abcdefghijklmnopqrstuvwxyz") == "abcdefg");
    REQUIRE(GitCheckpoint::short_sha("abc") == "abc");  // 已短于阈值：原样返回
    REQUIRE(GitCheckpoint::short_sha("") == "");
}

TEST_CASE("GitCheckpoint reset clears captured info", "[git_checkpoint]") {
    GitCheckpoint::instance().reset();
    REQUIRE_FALSE(GitCheckpoint::instance().info().valid);
    // 非仓库时统计为空，不崩溃
    REQUIRE(GitCheckpoint::instance().diff_since_base().files.empty());
}

TEST_CASE("GitCheckpoint captures base commit and lists later changes", "[git_checkpoint]") {
    TempRepo repo;
    if (!repo.ready) {
        SKIP("git 不可用，跳过临时仓库用例");
    }

    GitCheckpoint::instance().reset();
    REQUIRE(GitCheckpoint::instance().capture(repo.dir.string()));
    REQUIRE(GitCheckpoint::instance().info().valid);
    // 存完整 sha（40 位 sha-1 / 64 位 sha-256），避免大仓库短 sha 歧义
    REQUIRE(GitCheckpoint::instance().info().base_commit.size() >= 40);
    // 提交后工作区干净
    REQUIRE(GitCheckpoint::instance().info().clean_at_capture);

    // 改动：修改已跟踪文件 + 新增未跟踪文件
    write_file(repo.dir / "a.txt", "line1\nline2\n");
    write_file(repo.dir / "b.txt", "new\n");

    const auto summary = GitCheckpoint::instance().diff_since_base();
    REQUIRE(summary.files.size() == 2);
    REQUIRE(summary.total_insertions >= 1);

    bool saw_modified = false;
    bool saw_untracked = false;
    for (const auto& f : summary.files) {
        if (f.path == "a.txt" && f.status == "M") saw_modified = true;
        if (f.path == "b.txt" && f.status == "?") saw_untracked = true;
    }
    REQUIRE(saw_modified);
    REQUIRE(saw_untracked);

    // 回滚：仅恢复已跟踪文件，未跟踪的新文件不删除（删除不可逆）
    std::vector<std::string> skipped;
    const int done = GitCheckpoint::instance().rollback_tracked(summary, skipped);
    REQUIRE(done == 1);
    REQUIRE(skipped.size() == 1);
    REQUIRE(skipped.front() == "b.txt");
    REQUIRE(read_file(repo.dir / "a.txt") == "line1\n");
    REQUIRE(read_file(repo.dir / "b.txt") == "new\n");  // 未跟踪文件保持原样

    GitCheckpoint::instance().reset();
}

TEST_CASE("GitCheckpoint recaptures when the working directory changes", "[git_checkpoint]") {
    TempRepo repo_a;
    TempRepo repo_b;
    if (!repo_a.ready || !repo_b.ready) {
        SKIP("git 不可用，跳过临时仓库用例");
    }

    GitCheckpoint::instance().reset();
    REQUIRE(GitCheckpoint::instance().capture(repo_a.dir.string()));
    REQUIRE(GitCheckpoint::instance().capture(repo_b.dir.string()));
    REQUIRE(GitCheckpoint::instance().info().valid);
    // 目录变了必须重捕获：否则会拿 A 仓库的 base commit 去 diff / 回滚 B 仓库
    REQUIRE(fs::weakly_canonical(GitCheckpoint::instance().info().repo_root) ==
            fs::weakly_canonical(repo_b.dir));

    GitCheckpoint::instance().reset();
}

TEST_CASE("GitCheckpoint reports renamed files with their new path", "[git_checkpoint]") {
    TempRepo repo;
    if (!repo.ready) {
        SKIP("git 不可用，跳过临时仓库用例");
    }

    GitCheckpoint::instance().reset();
    REQUIRE(GitCheckpoint::instance().capture(repo.dir.string()));
    REQUIRE(git_run(repo.dir, {"mv", "a.txt", "c.txt"}));

    const auto summary = GitCheckpoint::instance().diff_since_base();
    bool saw_rename = false;
    for (const auto& f : summary.files) {
        // name-status 输出 "R100\told\tnew"，numstat 输出 "old => new"：统一取新路径
        if (f.path == "c.txt" && (f.status == "R" || f.status == "M")) saw_rename = true;
    }
    REQUIRE(saw_rename);

    GitCheckpoint::instance().reset();
}

TEST_CASE("GitCheckpoint rejects capture outside a git repository", "[git_checkpoint]") {
    GitCheckpoint::instance().reset();
    const auto dir = fs::temp_directory_path();
    // 临时目录通常不在仓库内；若在（极端情况）则跳过该断言
    if (GitCheckpoint::instance().capture(dir.string())) {
        SKIP("临时目录位于 git 仓库内，跳过非仓库用例");
    }
    REQUIRE_FALSE(GitCheckpoint::instance().info().valid);
    REQUIRE(GitCheckpoint::instance().diff_since_base().files.empty());
}
