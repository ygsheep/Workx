/**
 * @file test_file_permissions.cpp
 * @brief 私有文件/目录权限加固单元测试（Issue #88）
 * @details POSIX 断言权限位；Windows 断言 DACL 被 PROTECTED 且只剩「属主 + SYSTEM」两条 ACE。
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/config/config_manager.h"
#include "core/utils/file_permissions.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <aclapi.h>
#endif

namespace {

using agent::harden_private_dir;
using agent::harden_private_file;

/// 独占临时目录（析构时清理），避免污染用户真实配置目录
class TempDir {
   public:
    explicit TempDir(const std::string& name)
        : path(std::filesystem::temp_directory_path() / ("workx_perm_" + name)) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        // 目录可能已被收紧为仅属主，清理前先恢复属主全权，否则删不掉
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::filesystem::path path;
};

/// 造一个内容为 "{}" 的普通文件
void make_file(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "{}";
}

#if defined(_WIN32)

/// DACL 关键特征：加固成功后应为 2 条 ACE、置 PROTECTED、含 LocalSystem
struct DaclInfo {
    DWORD ace_count = 0;
    bool protected_dacl = false;
    bool has_local_system = false;
};

DaclInfo inspect_dacl(const std::filesystem::path& path) {
    DaclInfo info;
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return info;
    }

    ACL_SIZE_INFORMATION size_info{};
    if (dacl && GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation)) {
        info.ace_count = size_info.AceCount;
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (sd && GetSecurityDescriptorControl(sd, &control, &revision)) {
        info.protected_dacl = (control & SE_DACL_PROTECTED) != 0;
    }
    BYTE sys_sid[SECURITY_MAX_SID_SIZE];
    DWORD sys_len = sizeof(sys_sid);
    if (dacl && CreateWellKnownSid(WinLocalSystemSid, nullptr, sys_sid, &sys_len)) {
        for (DWORD i = 0; i < info.ace_count; ++i) {
            LPVOID entry = nullptr;
            if (GetAce(dacl, i, &entry) &&
                EqualSid(&static_cast<ACCESS_ALLOWED_ACE*>(entry)->SidStart, sys_sid)) {
                info.has_local_system = true;
            }
        }
    }
    LocalFree(sd);
    return info;
}

/// DACL 中是否存在「宽授权」ACE（Everyone / BUILTIN\Users / Authenticated Users）
/// @return 读不到 DACL 时返回 true（视为不安全）
bool has_broad_ace(const std::filesystem::path& path) {
    static const WELL_KNOWN_SID_TYPE kBroad[] = {WinWorldSid, WinBuiltinUsersSid,
                                                 WinAuthenticatedUserSid};
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return true;
    }
    bool broad = false;
    ACL_SIZE_INFORMATION size_info{};
    if (dacl && GetAclInformation(dacl, &size_info, sizeof(size_info), AclSizeInformation)) {
        for (DWORD i = 0; i < size_info.AceCount && !broad; ++i) {
            LPVOID entry = nullptr;
            if (!GetAce(dacl, i, &entry)) continue;
            PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(entry)->SidStart;
            for (WELL_KNOWN_SID_TYPE type : kBroad) {
                BYTE buf[SECURITY_MAX_SID_SIZE];
                DWORD len = sizeof(buf);
                if (CreateWellKnownSid(type, nullptr, buf, &len) && EqualSid(sid, buf)) {
                    broad = true;
                }
            }
        }
    }
    LocalFree(sd);
    return broad;
}

#else

/// 读回 0777 掩码下的权限位
unsigned mode_bits(const std::filesystem::path& path) {
    const auto mask = std::filesystem::perms::owner_all | std::filesystem::perms::group_all |
                      std::filesystem::perms::others_all;
    return static_cast<unsigned>(std::filesystem::status(path).permissions() & mask);
}

constexpr unsigned kFilePrivate = 0600;
constexpr unsigned kDirPrivate = 0700;

#endif

}  // namespace

TEST_CASE("file_permissions: 新文件收紧为仅属主可读写", "[file_permissions][issue88]") {
    TempDir dir("new_file");
    const auto file = dir.path / "config.json";
    make_file(file);

    REQUIRE(harden_private_file(file).is_ok());

#if defined(_WIN32)
    const auto info = inspect_dacl(file);
    CHECK(info.ace_count == 2);
    CHECK(info.protected_dacl);
    CHECK(info.has_local_system);
    CHECK_FALSE(has_broad_ace(file));
#else
    CHECK(mode_bits(file) == kFilePrivate);
#endif
}

TEST_CASE("file_permissions: 已存在的过宽文件被就地收紧", "[file_permissions][issue88]") {
    TempDir dir("widen_file");
    const auto file = dir.path / "config.json";
    make_file(file);

#if defined(_WIN32)
    // 与 save_to_file 同样顺序：二次写入(trunc)不得撤销已加固的 DACL
    REQUIRE(harden_private_file(file).is_ok());
    make_file(file);
    REQUIRE(harden_private_file(file).is_ok());
    const auto info = inspect_dacl(file);
    CHECK(info.protected_dacl);
    CHECK(info.ace_count == 2);
    CHECK_FALSE(has_broad_ace(file));
#else
    // 模拟老用户历史遗留的 0644：加固必须就地收紧，而不是只在创建路径生效
    // 注意 owner_all 含 owner_exec（0700），这里要的是 owner_read|owner_write
    std::filesystem::permissions(
        file, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
                  std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    REQUIRE(mode_bits(file) == 0644);
    REQUIRE(harden_private_file(file).is_ok());
    CHECK(mode_bits(file) == kFilePrivate);
#endif
}

TEST_CASE("file_permissions: 目录收紧为仅属主可访问", "[file_permissions][issue88]") {
    TempDir dir("new_dir");
    const auto sub = dir.path / "cfg";
    std::filesystem::create_directories(sub);

    REQUIRE(harden_private_dir(sub).is_ok());

#if defined(_WIN32)
    const auto info = inspect_dacl(sub);
    CHECK(info.ace_count == 2);
    CHECK(info.protected_dacl);
    CHECK_FALSE(has_broad_ace(sub));
#else
    CHECK(mode_bits(sub) == kDirPrivate);
#endif
}

TEST_CASE("file_permissions: 目标不存在返回 ResourceNotFound", "[file_permissions][issue88]") {
    TempDir dir("missing");
    const auto file_result = harden_private_file(dir.path / "nope.json");
    REQUIRE(file_result.is_err());
    CHECK(file_result.error().code == agent::Error::Code::ResourceNotFound);

    const auto dir_result = harden_private_dir(dir.path / "no_such_dir");
    REQUIRE(dir_result.is_err());
    CHECK(dir_result.error().code == agent::Error::Code::ResourceNotFound);
}

TEST_CASE("file_permissions: save_to_file 落盘后收紧权限", "[file_permissions][issue88]") {
    TempDir dir("config_save");
    const auto file = dir.path / "config.json";
    auto& cfg = agent::ConfigManager::instance();
    cfg.clear();
    cfg.set("backend.api_key", std::string("sk-secret"));

    REQUIRE(cfg.save_to_file(file).is_ok());

#if defined(_WIN32)
    const auto info = inspect_dacl(file);
    CHECK(info.protected_dacl);
    CHECK(info.ace_count == 2);
    CHECK_FALSE(has_broad_ace(file));
#else
    CHECK(mode_bits(file) == kFilePrivate);
    CHECK(mode_bits(dir.path) == kDirPrivate);
#endif

    std::filesystem::remove(file);
    cfg.clear();
}
