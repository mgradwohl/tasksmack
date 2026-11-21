#pragma once

// Who owns a file or folder, and who that owner is, read in one place (#4368): the elevated code that decides whether
// to trust a folder or an install (ElevatedRestart, HandleRelativeFile, WindowsDumpFolder, BrokerHelper) asks here
// instead of keeping its own GetSecurityInfo call, well-known SID checks and TrustedInstaller SID literal.
//   - ownerSidOf(): a handle's owner SID, read through the handle (GetSecurityInfo), with the descriptor it lives in.
//   - Named predicates over a SID: isAdministrators(), isAdministratorsOrSystem(), isTrustedInstaller() and
//     isTokenUser(). Each caller composes the set of owners it trusts from these, so the set stays visible where the
//     decision is made.
//   - TrustedInstaller's SID, one literal. The callers differ in when it is converted (each call, or once and kept),
//     so both forms are here, named for it, and each caller keeps the one it had.
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
#include <optional>
#include <string_view>

namespace Platform::Windows
{

/// NT SERVICE\TrustedInstaller's service SID, which owns and may change what Windows itself installs.
inline constexpr std::wstring_view TRUSTED_INSTALLER_SID = L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464";

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

/// Whether @p sid is TrustedInstaller, against its SID converted from TRUSTED_INSTALLER_SID for this call
/// (ConvertStringSidToSidW, which allocates); nullopt when that conversion fails, for the caller to decide.
[[nodiscard]] std::optional<bool> isTrustedInstaller(PSID sid);

/// Whether @p sid is TrustedInstaller, against its SID converted once, at the first call, and kept for the process's
/// life; nullopt at this and every later call when that one conversion failed.
[[nodiscard]] std::optional<bool> isTrustedInstallerConvertedOnce(PSID sid);

/// Whether @p sid is this process token's user (TokenUser, read now through the current-process token pseudo-handle by
/// readTokenUserSid(), WindowsElevation.h); the Win32 error when the token's user can't be read.
[[nodiscard]] std::expected<bool, DWORD> isTokenUser(PSID sid);

} // namespace Platform::Windows

#endif // _WIN32
