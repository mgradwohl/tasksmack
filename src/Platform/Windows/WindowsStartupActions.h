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
#include "WindowsNativeIds.h"
#include "WindowsScheduledTasks.h"

#include <memory>

namespace Platform
{

namespace Windows
{

/// The calls WindowsStartupActions makes: advapi32's (and kernel32's clock), or a test's fakes.
struct StartupRegistryFunctions
{
    using RegCreateKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, LPWSTR, DWORD, REGSAM, LPSECURITY_ATTRIBUTES, PHKEY, LPDWORD);
    /// RegOpenKeyExW: a packaged startup task's key is opened, never created (#1873).
    using RegOpenKeyExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, REGSAM, PHKEY);
    using RegQueryValueExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
    using RegSetValueExFn = LSTATUS(WINAPI*)(HKEY, LPCWSTR, DWORD, DWORD, const BYTE*, DWORD);
    using RegCloseKeyFn = LSTATUS(WINAPI*)(HKEY);
    using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);

    RegCreateKeyExFn createKey = &::RegCreateKeyExW;
    RegOpenKeyExFn openKey = &::RegOpenKeyExW;
    RegQueryValueExFn queryValue = &::RegQueryValueExW;
    RegSetValueExFn setValue = &::RegSetValueExW;
    RegCloseKeyFn closeKey = &::RegCloseKey;
    GetSystemTimeAsFileTimeFn now = &::GetSystemTimeAsFileTime;
};

} // namespace Windows

/// Writes StartupApproved values under HKCU or HKLM (Run, Run32 or StartupFolder, by the entry's
/// location), keeping any bytes of an existing value it doesn't own. An all-users entry needs
/// elevation; refused, it reports "Requires administrator". RunOnce entries are refused: they have no
/// StartupApproved state.
///
/// A packaged app's startup task (#1873) is changed as Settings > Apps > Startup changes it: its State
/// under StartupMath::PACKAGED_TASKS_KEY\<family>\<TaskId> becomes Enabled (2), or DisabledByUser (1)
/// with LastDisabledTime set to now (Unix seconds); nothing else is written. The key is opened, never
/// created, and a task whose State a policy sets (3 or 4) is refused.
///
/// A scheduled task (#3424) is switched through the Task Scheduler API, IRegisteredTask::put_Enabled, inside a
/// ComScope of the call's own: never deleted, and one of Windows' own (under \Microsoft\) refused
/// (Windows::setStartupTaskEnabled). Stateless beyond its fixed function tables, so safe from any thread.
class WindowsStartupActions final : public IStartupActions
{
  public:
    /// @param elevated Whether TaskSmack runs elevated, for capabilities().elevated.
    /// @param scheduledTasks Makes the Task Scheduler API a scheduled task's switch uses (tests: a fake).
    /// @param ids The table WindowsStartupProbe publishes its entries' exact identities into (#4304): an entry's
    /// StartupEntry::nativeId is resolved through it, and one no longer listed is refused. An entry with no id (built
    /// by the caller) is switched by its UTF-8 names, unless they are lossy (Windows::exactWideName()).
    explicit WindowsStartupActions(bool elevated,
                                   Windows::StartupRegistryFunctions api = {},
                                   Windows::ScheduledTaskApiFactory scheduledTasks = &Windows::makeTaskSchedulerApi,
                                   std::shared_ptr<const Windows::StartupIdTable> ids = Windows::sharedStartupIds());

    [[nodiscard]] StartupActionCapabilities capabilities() const override;
    [[nodiscard]] StartupActionResult setEnabled(const StartupEntry& entry, bool enabled) override;

  private:
    bool m_Elevated;
    Windows::StartupRegistryFunctions m_Api;
    Windows::ScheduledTaskApiFactory m_ScheduledTasks;
    std::shared_ptr<const Windows::StartupIdTable> m_Ids;
};

} // namespace Platform

#endif // _WIN32
