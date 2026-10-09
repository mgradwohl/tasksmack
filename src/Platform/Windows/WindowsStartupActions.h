#pragma once

// Enabling and disabling startup entries the way Task Manager does (#801, phase 2): by writing the
// entry's Explorer\StartupApproved value, never by deleting or moving its Run value or shortcut. The
// registry calls are a table of function pointers, advapi32's by default, so tests drive every path
// with fakes (test_WindowsStartupActions.cpp) and never write the real registry.

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

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"

namespace Platform
{

namespace Windows
{

/// The calls WindowsStartupActions makes: advapi32's (and kernel32's clock), or a test's fakes.
struct StartupRegistryFunctions
{
    using RegCreateKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
    using RegQueryValueExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    using RegSetValueExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
    using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
    using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);

    RegCreateKeyExFn createKey = &::RegCreateKeyExW;
    RegQueryValueExFn queryValue = &::RegQueryValueExW;
    RegSetValueExFn setValue = &::RegSetValueExW;
    RegCloseKeyFn closeKey = &::RegCloseKey;
    GetSystemTimeAsFileTimeFn now = &::GetSystemTimeAsFileTime;
};

} // namespace Windows

/// Writes StartupApproved values under HKCU or HKLM (Run, Run32 or StartupFolder, by the entry's
/// location), keeping any bytes of an existing value it doesn't own. An all-users entry needs
/// elevation; refused, it reports "Requires administrator". RunOnce entries are refused: they have no
/// StartupApproved state. Stateless beyond its fixed function table, so safe from any thread.
class WindowsStartupActions final : public IStartupActions
{
  public:
    /// @param elevated Whether TaskSmack runs elevated, for capabilities().elevated.
    explicit WindowsStartupActions(bool elevated, Windows::StartupRegistryFunctions api = {});

    [[nodiscard]] StartupActionCapabilities capabilities() const override;
    [[nodiscard]] StartupActionResult setEnabled(const StartupEntry& entry, bool enabled) override;

  private:
    bool m_Elevated;
    Windows::StartupRegistryFunctions m_Api;
};

} // namespace Platform

#endif // _WIN32
