/**
 * @file mock_git_repo.h
 * @brief 测试用临时 git 仓库 fixture（Issue #77 HL-12 / #81 用例）
 * @details 在系统临时目录下建一个独立 git 仓库，析构时递归删除。
 *          所有 git 调用走 `git -C <dir>`，不依赖进程当前目录。
 *
 * 使用示例：
 * @code
 *   using namespace agent::test;
 *   MockGitRepo repo("hl12");
 *   if (!repo.ok()) SKIP("git 不可用");
 *   repo.write_file("a.txt", "one\n");
 *   repo.commit_all("init");
 *   std::filesystem::current_path(repo.path());   // 需要自行切换并恢复
 *   // ... 被测代码 ...
 * @endcode
 */

#pragma once

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>

namespace agent::test {

/// @brief RAII 临时 git 仓库
/// @details 需要 PATH 上有 git；否则 ok() 返回 false，用例应 SKIP。
class MockGitRepo {
   public:
    explicit MockGitRepo(const std::string& tag = "repo") {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        dir_ = std::filesystem::temp_directory_path() /
               ("workx_gittest_" + tag + "_" + std::to_string(stamp));

        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        if (ec) return;

        // init + 最小身份配置（不依赖全局 git config，避免 CI 上 user.name 缺失）
        if (sh({"init", "-q"}) != 0) return;
        sh({"config", "user.email", "test@workx.local"});
        sh({"config", "user.name", "Workx Test"});
        sh({"config", "commit.gpgsign", "false"});
        ok_ = true;
    }

    ~MockGitRepo() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    MockGitRepo(const MockGitRepo&) = delete;
    MockGitRepo& operator=(const MockGitRepo&) = delete;

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] const std::filesystem::path& path() const { return dir_; }
    [[nodiscard]] std::string path_str() const { return dir_.generic_string(); }

    /// @brief 执行 git 子命令（丢弃输出），返回退出码
    int sh(std::initializer_list<std::string> args) const {
        std::string cmd = "git -C \"" + path_str() + "\"";
        for (const auto& a : args) cmd += " " + a;
#ifdef _WIN32
        cmd += " > NUL 2>&1";
#else
        cmd += " > /dev/null 2>&1";
#endif
        return std::system(cmd.c_str());
    }

    /// @brief 写入（或覆盖）仓库内文件
    bool write_file(const std::string& rel, const std::string& content) const {
        std::ofstream f(dir_ / rel, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << content;
        return f.good();
    }

    /// @brief 删除仓库内文件
    bool remove_file(const std::string& rel) const {
        std::error_code ec;
        return std::filesystem::remove(dir_ / rel, ec);
    }

    /// @brief 暂存全部改动并提交
    bool commit_all(const std::string& message) const {
        if (sh({"add", "-A"}) != 0) return false;
        return sh({"commit", "-q", "-m", "\"" + message + "\""}) == 0;
    }

    /// @brief 当前 HEAD 的完整 sha（失败返回空串）
    std::string head_sha() const {
        const std::string cmd = "git -C \"" + path_str() + "\" rev-parse HEAD 2>&1";
        FILE* pipe = _popen(cmd.c_str(), "r");
        if (!pipe) return {};
        char buf[128] = {0};
        std::string out;
        if (std::fgets(buf, sizeof(buf), pipe) != nullptr) out = buf;
        _pclose(pipe);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        return out;
    }

   private:
    std::filesystem::path dir_;
    bool ok_ = false;
};

}  // namespace agent::test
