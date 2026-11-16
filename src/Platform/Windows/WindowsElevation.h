#pragma once

// What TaskSmack's own process token says, read in one place (#2275, #3864): whether it is elevated
// (TokenElevation), its elevation type (TokenElevationType), and its user (TokenUser), as a SID and as
// the account LookupAccountSidW names. Every Windows Platform module that gates on elevation or names
// TaskSmack's own account asks here: the process probe (TCP EStats), the ETW UDP collector,
// SeDebugPrivilege at startup, the service and startup actions' capabilities, the elevated restart,
// the helper launcher, Open file location, Run new task, the session-account check, the Startup tab's
// sign-in measurements and its scheduled tasks.
//
// None of it changes while the process runs, so isProcessElevated() reads the token once and keeps the
// answer. The token calls are a table of function pointers, advapi32's by default, so tests drive every
// answer and every failure with fakes (test_WindowsElevation.cpp).

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

#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace Platform::Windows
{

/// The calls the current-token reads make: advapi32's, or a test's fakes.
struct CurrentTokenFunctions
{
    using OpenProcessTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using GetTokenInformationFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    using LookupAccountSidFn = BOOL(WINAPI*)(LPCWSTR, PSID, LPWSTR, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE);

    OpenProcessTokenFn openProcessToken = &::OpenProcessToken;
    GetTokenInformationFn getTokenInformation = &::GetTokenInformation;
    LookupAccountSidFn lookupAccountSid = &::LookupAccountSidW;
};

/// An account as Windows names it: its domain (the computer's name for a local account, "AzureAD" for an
/// Entra ID one) and its user name.
struct AccountName
{
    std::wstring domain;
    std::wstring user;
};

/// TokenElevation of an already-open token (opened with at least TOKEN_QUERY): whether it is
/// elevated, or empty if it can't be read. Logs nothing and leaves GetLastError() as
/// getTokenInformation set it, so a caller holding the token for more (SeDebugPrivilege) reports
/// the failure itself.
[[nodiscard]] std::optional<bool> readTokenElevation(HANDLE token, CurrentTokenFunctions::GetTokenInformationFn getTokenInformation);

/// The user SID of an already-open token (TOKEN_QUERY), as its bytes, copied out of its TOKEN_USER
/// (BrokerPipe::Sid's form); the Win32 error when it can't be read, ERROR_INVALID_SID when the token's
/// user isn't a valid SID.
[[nodiscard]] std::expected<std::vector<std::byte>, DWORD>
readTokenUserSid(HANDLE token, CurrentTokenFunctions::GetTokenInformationFn getTokenInformation);

/// The account @p sid names, through @p lookupAccountSid on the local computer, its buffers sized by
/// the call itself however long the names are (#3864); nullopt when it can't be named.
[[nodiscard]] std::optional<AccountName> lookupAccountName(PSID sid, CurrentTokenFunctions::LookupAccountSidFn lookupAccountSid);

/// Whether the current process token is elevated, read now through @p fns: opens it with TOKEN_QUERY and
/// reads TokenElevation. Conservative: any failure reports "not elevated" (logged at debug level).
[[nodiscard]] bool readProcessElevated(const CurrentTokenFunctions& fns);

/// readProcessElevated() with the real calls, read once and kept for the process's lifetime: a token's
/// elevation never changes. Thread-safe.
[[nodiscard]] bool isProcessElevated();

/// The current process token's TokenElevationType; TokenElevationTypeDefault when it can't be read.
[[nodiscard]] TOKEN_ELEVATION_TYPE currentElevationType(const CurrentTokenFunctions& fns = {}) noexcept;

/// The current process token's user SID, as readTokenUserSid() gives it; the Win32 error when the token
/// can't be opened or read.
[[nodiscard]] std::expected<std::vector<std::byte>, DWORD> currentUserSid(const CurrentTokenFunctions& fns = {});

/// currentUserSid() as text ("S-1-5-21-..."); the Win32 error when it can't be read or converted.
[[nodiscard]] std::expected<std::wstring, DWORD> currentUserSidText(const CurrentTokenFunctions& fns = {});

/// The account of the current process token's user, by name (lookupAccountName()); nullopt when the
/// token can't be read or its user can't be named (logged at debug level).
[[nodiscard]] std::optional<AccountName> currentUserName(const CurrentTokenFunctions& fns = {});

} // namespace Platform::Windows

#endif // _WIN32
