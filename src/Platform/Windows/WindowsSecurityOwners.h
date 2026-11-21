#pragma once

// Who owns a file or folder, and who that owner is, read in one place (#4368): the elevated code that decides whether
// to trust a folder or an install (ElevatedRestart, HandleRelativeFile, WindowsDumpFolder, BrokerHelper) asks here
// instead of keeping its own GetSecurityInfo call, well-known SID checks and TrustedInstaller SID.
//   - ownerSidOf(): a handle's owner SID, read through the handle (GetSecurityInfo), with the descriptor it lives in.
//   - Named predicates over a SID: isAdministrators(), isAdministratorsOrSystem(), isTrustedInstaller() and
//     isTokenUser(). Each caller composes the set of owners it trusts from these, so the set stays visible where the
//     decision is made.
//   - TrustedInstaller's SID, trustedInstallerSid(): one static SID built from its known parts (#4699), so it is never
//     converted from text, never allocates and can't fail.
// TaskSmackHelper.exe links this, so it includes nothing of the shell or COM.

#ifdef _WIN32
// clang-format off
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// clang-format on

#include <expected>
#include <memory>

namespace Platform::Windows
{

/// A handle's owner, as ownerSidOf() read it.
struct HandleOwner
{
    /// The security descriptor GetSecurityInfo allocated (LocalFree'd when this dies); @c sid points into it.
    std::unique_ptr<void, decltype(&::LocalFree)> descriptor{nullptr, &::LocalFree};
    /// The owner SID, valid only while @c descriptor lives.
    PSID sid = nullptr;
};

/// The owner of the file or folder @p handle holds (opened with READ_CONTROL), read through that handle
/// (GetSecurityInfo, OWNER_SECURITY_INFORMATION). The Win32 error when it can't be read, ERROR_INVALID_OWNER when the
/// descriptor names no owner.
[[nodiscard]] std::expected<HandleOwner, DWORD> ownerSidOf(HANDLE handle);

/// Whether @p sid is BUILTIN\Administrators.
[[nodiscard]] bool isAdministrators(PSID sid) noexcept;

/// Whether @p sid is BUILTIN\Administrators or SYSTEM (LocalSystem).
[[nodiscard]] bool isAdministratorsOrSystem(PSID sid) noexcept;

/// NT SERVICE\TrustedInstaller's service SID, S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464, which
/// owns and may change what Windows itself installs: a static SID built from its parts (SECURITY_NT_AUTHORITY,
/// SECURITY_SERVICE_ID_BASE_RID and the five sub-authorities hashed from "TrustedInstaller"), valid for the process's
/// life. Read-only: the Win32 calls that take a PSID only read it, and nothing may write through it.
[[nodiscard]] PSID trustedInstallerSid() noexcept;

/// Whether @p sid is TrustedInstaller (trustedInstallerSid()); false for a null or invalid SID.
[[nodiscard]] bool isTrustedInstaller(PSID sid) noexcept;

/// Whether @p sid is this process token's user (TokenUser, read now through the current-process token pseudo-handle by
/// readTokenUserSid(), WindowsElevation.h); the Win32 error when the token's user can't be read.
[[nodiscard]] std::expected<bool, DWORD> isTokenUser(PSID sid);

} // namespace Platform::Windows

#endif // _WIN32
