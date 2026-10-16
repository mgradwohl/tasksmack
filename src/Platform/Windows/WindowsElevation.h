#pragma once

// Whether TaskSmack's own process token is elevated (TokenElevation), the one check every Windows
// Platform module that gates on elevation shares (#2275): the process probe (TCP EStats), the ETW
// UDP collector, SeDebugPrivilege at startup, and the service and startup actions' capabilities.
// Each caller decides when to ask: the answer is constant for the process lifetime, so most read it
// once at construction.
//
// The token calls are a table of function pointers, advapi32's by default, so tests drive both
// answers and every failure with fakes (test_WindowsElevation.cpp).

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

#include <optional>

namespace Platform::Windows
{

/// The calls isProcessElevated() makes: advapi32's, or a test's fakes.
struct ElevationFunctions
{
    using OpenProcessTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using GetTokenInformationFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);

    OpenProcessTokenFn openProcessToken = &::OpenProcessToken;
    GetTokenInformationFn getTokenInformation = &::GetTokenInformation;
};

/// TokenElevation of an already-open token (opened with at least TOKEN_QUERY): whether it is
/// elevated, or empty if it can't be read. Logs nothing and leaves GetLastError() as
/// getTokenInformation set it, so a caller holding the token for more (SeDebugPrivilege) reports
/// the failure itself.
[[nodiscard]] std::optional<bool> readTokenElevation(HANDLE token, ElevationFunctions::GetTokenInformationFn getTokenInformation);

/// Whether the current process token is elevated: opens it with TOKEN_QUERY and reads
/// TokenElevation. Conservative: any failure reports "not elevated" (logged at debug level).
[[nodiscard]] bool isProcessElevated(const ElevationFunctions& fns = {});

} // namespace Platform::Windows

#endif // _WIN32
