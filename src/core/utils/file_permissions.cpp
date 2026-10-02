/**
 * @file file_permissions.cpp
 * @brief 私有文件/目录权限加固实现
 */

#include "core/utils/file_permissions.h"

#include <format>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <aclapi.h>
#else
#include <sys/stat.h>

#include <cerrno>
#include <cstring>
#endif

namespace agent {

#if defined(_WIN32)

namespace {

/// 读取路径的属主 SID（LocalAlloc 副本，调用方须 LocalFree）
/// @return 失败返回 nullptr
PSID read_owner_sid(const std::filesystem::path& path) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                              nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS) {
        return nullptr;
    }
    const DWORD len = owner ? GetLengthSid(owner) : 0;
    PSID copy = len > 0 ? LocalAlloc(LPTR, len) : nullptr;
    if (copy) {
        CopySid(len, copy, owner);
    }
    LocalFree(sd);
    return copy;
}

/// 构造「属主 + SYSTEM」两条 ACE 的 DACL（调用方须 LocalFree）
/// @return 失败返回 nullptr
PACL build_private_dacl(PSID owner, bool is_dir) {
    BYTE sys_buf[SECURITY_MAX_SID_SIZE];
    DWORD sys_len = sizeof(sys_buf);
    if (!CreateWellKnownSid(WinLocalSystemSid, nullptr, sys_buf, &sys_len)) {
        return nullptr;
    }

    EXPLICIT_ACCESS_W ea[2];
    ZeroMemory(ea, sizeof(ea));
    ea[0].grfAccessPermissions = FILE_ALL_ACCESS;
    ea[0].grfAccessMode = SET_ACCESS;
    ea[0].grfInheritance = is_dir ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
    ea[0].Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea[0].Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea[0].Trustee.ptstrName = static_cast<LPWSTR>(owner);
    ea[1] = ea[0];
    ea[1].Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea[1].Trustee.ptstrName = reinterpret_cast<LPWSTR>(sys_buf);

    PACL acl = nullptr;
    return SetEntriesInAclW(2, ea, nullptr, &acl) == ERROR_SUCCESS ? acl : nullptr;
}

ResultV2<void> harden_windows(const std::filesystem::path& path, bool is_dir) {
    const std::string ctx = path.string();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return ResultV2<void>::err(Error::Code::ResourceNotFound, "harden target not found", ctx);
    }

    PSID owner = read_owner_sid(path);
    if (!owner) {
        return ResultV2<void>::err(Error::Code::PermissionDenied, "cannot read owner SID", ctx);
    }
    PACL acl = build_private_dacl(owner, is_dir);
    LocalFree(owner);
    if (!acl) {
        return ResultV2<void>::err(Error::Code::PermissionDenied, "cannot build private DACL", ctx);
    }

    // PROTECTED 标志是必须的：缺它则父目录继承 ACE 会被重新注入，加固被撤销。
    const DWORD rc =
        SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    if (rc != ERROR_SUCCESS) {
        return ResultV2<void>::err(Error::Code::PermissionDenied,
                                   std::format("SetNamedSecurityInfoW failed: {}", rc), ctx);
    }
    return ResultV2<void>::ok();
}

/// @brief 查询 DACL 是否只向「属主 + SYSTEM」授权
/// @details 与 build_private_dacl 的产出对齐：任何**会授予访问**的 ACE 都必须属于这两个主体，
///          其余一律判为过宽。空 DACL 意味着 everyone 全权，比任何显性授权都更开放，同样判过宽。
///          DENY ACE 只**收回**权限，不参与判定 —— 一条针对陌生主体的 DENY 不会让文件变宽。
ResultV2<bool> inspect_dacl_privacy(const std::filesystem::path& path) {
    const std::string ctx = path.string();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                              nullptr, &dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return ResultV2<bool>::err(Error::Code::PermissionDenied, "cannot read DACL", ctx);
    }
    if (dacl == nullptr) {
        LocalFree(sd);
        return ResultV2<bool>::ok(false);
    }

    ACL_SIZE_INFORMATION size{};
    if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation)) {
        LocalFree(sd);
        return ResultV2<bool>::err(Error::Code::PermissionDenied, "cannot read ACL size", ctx);
    }
    PSID owner = read_owner_sid(path);
    BYTE sys_buf[SECURITY_MAX_SID_SIZE];
    DWORD sys_len = sizeof(sys_buf);
    const bool sys_ok = CreateWellKnownSid(WinLocalSystemSid, nullptr, sys_buf, &sys_len) == TRUE;
    if (!owner || !sys_ok) {
        LocalFree(owner);
        LocalFree(sd);
        return ResultV2<bool>::err(Error::Code::PermissionDenied, "cannot resolve trusted SIDs",
                                   ctx);
    }

    bool trusted_only = true;
    for (DWORD i = 0; i < size.AceCount && trusted_only; ++i) {
        LPVOID entry = nullptr;
        if (!GetAce(dacl, i, &entry)) continue;
        if (static_cast<ACE_HEADER*>(entry)->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        // ALLOWED 与 OBJECT_ALLOWED 在 SidStart 处布局一致，统一取这一段
        PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(entry)->SidStart;
        if (!EqualSid(sid, owner) && !EqualSid(sid, sys_buf)) trusted_only = false;
    }
    LocalFree(owner);
    LocalFree(sd);
    return ResultV2<bool>::ok(trusted_only);
}

}  // namespace

#else  // POSIX

namespace {

ResultV2<void> harden_posix(const std::filesystem::path& path, mode_t mode) {
    const std::string ctx = path.string();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return ResultV2<void>::err(Error::Code::ResourceNotFound, "harden target not found", ctx);
    }
    if (::chmod(path.c_str(), mode) != 0) {
        return ResultV2<void>::err(Error::Code::PermissionDenied,
                                   std::format("chmod({:o}) failed: {}",
                                               static_cast<unsigned>(mode), std::strerror(errno)),
                                   ctx);
    }
    return ResultV2<void>::ok();
}

/// @brief 查询权限位是否已无 group / others 授权
/// @details 与 harden_posix 的目标位对齐：group 与 others 的 rwx 全部为 0 才算收紧。
///          注意 perms::*_all 都含执行位，掩码必须覆盖读写执行三者。
ResultV2<bool> inspect_posix_privacy(const std::filesystem::path& path) {
    const std::string ctx = path.string();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return ResultV2<bool>::err(Error::Code::ResourceNotFound, "target not found", ctx);
    }
    const auto perms = std::filesystem::status(path, ec).permissions();
    if (ec) {
        return ResultV2<bool>::err(Error::Code::PermissionDenied, "cannot read permissions", ctx);
    }
    const auto others = std::filesystem::perms::group_all | std::filesystem::perms::others_all;
    return ResultV2<bool>::ok((perms & others) == std::filesystem::perms::none);
}

}  // namespace

#endif

ResultV2<void> harden_private_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return harden_windows(path, /*is_dir=*/false);
#else
    return harden_posix(path, 0600);
#endif
}

ResultV2<void> harden_private_dir(const std::filesystem::path& path) {
#if defined(_WIN32)
    return harden_windows(path, /*is_dir=*/true);
#else
    return harden_posix(path, 0700);
#endif
}

ResultV2<bool> is_private_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    return inspect_dacl_privacy(path);
#else
    return inspect_posix_privacy(path);
#endif
}

}  // namespace agent
