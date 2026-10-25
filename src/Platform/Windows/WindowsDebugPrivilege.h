#pragma once

// SeDebugPrivilege for an elevated TaskSmack (#2128). An administrator's elevated token holds the
// privilege but disabled, and without it the access checks on processes running as LocalSystem,
// LocalService or NetworkService go by their default DACL, which grants Administrators no right to
// terminate, set information on, duplicate handles from or snapshot them. Task Manager and Process
// Explorer enable it for this reason; TaskSmack does too, once at startup (main.cpp, through
// Platform::enableDebugPrivilege()). It does not bypass protected processes (PPL), whose refusals keep
// their own messages. Not elevated, nothing is changed.
//
// The token calls are a table of function pointers, advapi32's by default, so tests drive every
// outcome with fakes (test_WindowsDebugPrivilege.cpp) and never adjust a real token.

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

#include <cstdint>

namespace Platform::Windows
{

/// The calls enableDebugPrivilegeIfElevated() makes: advapi32's, or a test's fakes.
struct DebugPrivilegeFunctions
{
    using OpenProcessTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using GetTokenInformationFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    using LookupPrivilegeValueFn = BOOL(WINAPI*)(LPCWSTR, LPCWSTR, PLUID);
    using AdjustTokenPrivilegesFn = BOOL(WINAPI*)(HANDLE, BOOL, PTOKEN_PRIVILEGES, DWORD, PTOKEN_PRIVILEGES, PDWORD);

    OpenProcessTokenFn openProcessToken = &::OpenProcessToken;
    GetTokenInformationFn getTokenInformation = &::GetTokenInformation;
    LookupPrivilegeValueFn lookupPrivilegeValue = &::LookupPrivilegeValueW;
    AdjustTokenPrivilegesFn adjustTokenPrivileges = &::AdjustTokenPrivileges;
};

/// What enableDebugPrivilegeIfElevated() did.
enum class DebugPrivilegeResult : std::uint8_t
{
    Enabled,          ///< Elevated, and SeDebugPrivilege is now enabled.
    NotElevated,      ///< Not elevated: nothing was changed (an unelevated token has no SeDebugPrivilege).
    TokenUnavailable, ///< The process token couldn't be opened or its elevation read.
    LookupFailed,     ///< LookupPrivilegeValueW(SE_DEBUG_NAME) failed.
    AdjustFailed,     ///< AdjustTokenPrivileges failed.
    NotAssigned,      ///< AdjustTokenPrivileges succeeded with ERROR_NOT_ALL_ASSIGNED: the token lacks the privilege.
};

/// Enables SeDebugPrivilege on TaskSmack's own token when it is elevated, and logs the outcome. Opens
/// the token with TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, reads TokenElevation and, only when elevated,
/// looks up SE_DEBUG_NAME and enables it with AdjustTokenPrivileges, checking GetLastError() for
/// ERROR_NOT_ALL_ASSIGNED (the call "succeeds" without enabling anything when the token lacks the
/// privilege). Every failure is logged and returned, never thrown: startup goes on without it.
[[nodiscard]] DebugPrivilegeResult enableDebugPrivilegeIfElevated(const DebugPrivilegeFunctions& fns = {});

} // namespace Platform::Windows

#endif // _WIN32
