#pragma once

// Windows IProcessSecurityReader (#1526): the process token. OpenProcess with
// PROCESS_QUERY_LIMITED_INFORMATION (its creation time confirms the target's identity), then
// OpenProcessToken with TOKEN_QUERY and GetTokenInformation for the token's user, integrity level,
// elevation and elevation type, privileges, groups, AppContainer and virtualization state, and session.
// SIDs are named with LookupAccountSidW (cached by SID), falling back to the SID string; privileges
// with LookupPrivilegeNameW. The calls it makes are a table of function pointers, the system's by
// default, so tests feed it fixed tokens and denials (test_WindowsProcessSecurity.cpp).

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

#include "Platform/IProcessActions.h"
#include "Platform/IProcessSecurity.h"

#include <cstddef>
#include <string>
#include <unordered_map>

namespace Platform::Windows
{

/// The calls WindowsProcessSecurityReader makes: kernel32's and advapi32's, or a test's fakes. Every
/// handle the open calls return is closed with the real CloseHandle (UniqueHandle), so a fake must
/// hand out real handles (CreateEventW).
struct ProcessSecurityFunctions
{
    using OpenProcessFn = HANDLE(WINAPI*)(DWORD, BOOL, DWORD);
    using GetProcessTimesFn = BOOL(WINAPI*)(HANDLE, LPFILETIME, LPFILETIME, LPFILETIME, LPFILETIME);
    using OpenProcessTokenFn = BOOL(WINAPI*)(HANDLE, DWORD, PHANDLE);
    using GetTokenInformationFn = BOOL(WINAPI*)(HANDLE, TOKEN_INFORMATION_CLASS, LPVOID, DWORD, PDWORD);
    using LookupAccountSidFn = BOOL(WINAPI*)(LPCWSTR, PSID, LPWSTR, LPDWORD, LPWSTR, LPDWORD, PSID_NAME_USE);
    using LookupPrivilegeNameFn = BOOL(WINAPI*)(LPCWSTR, PLUID, LPWSTR, LPDWORD);

    OpenProcessFn openProcess = &::OpenProcess;
    GetProcessTimesFn getProcessTimes = &::GetProcessTimes;
    OpenProcessTokenFn openProcessToken = &::OpenProcessToken;
    GetTokenInformationFn getTokenInformation = &::GetTokenInformation;
    LookupAccountSidFn lookupAccountSid = &::LookupAccountSidW;
    LookupPrivilegeNameFn lookupPrivilegeName = &::LookupPrivilegeNameW;
};

/// @p sid in its string form ("S-1-5-32-544"), as ConvertSidToStringSidW writes it; empty for an
/// invalid SID.
[[nodiscard]] std::string sidToString(PSID sid);

/// Not thread-safe: one read at a time (the Security section reads from the panel's update path).
class WindowsProcessSecurityReader final : public IProcessSecurityReader
{
  public:
    WindowsProcessSecurityReader() = default;

    /// Test seam: make every call through @p api.
    explicit WindowsProcessSecurityReader(const ProcessSecurityFunctions& api) : m_Api(api)
    {}

    [[nodiscard]] bool hasSecurity() const override;
    [[nodiscard]] SecurityReadResult readSecurity(const ProcessTarget& target) override;

    /// How many account names are cached (tests: a SID is looked up once).
    [[nodiscard]] std::size_t cachedAccountCount() const noexcept
    {
        return m_AccountNames.size();
    }

  private:
    /// @p sid as an account: its string form, and its name, looked up once per SID and cached.
    [[nodiscard]] SecurityAccount account(PSID sid);

    ProcessSecurityFunctions m_Api;
    std::unordered_map<std::string, std::string> m_AccountNames; ///< SID string -> "DOMAIN\name" (empty: none).
};

} // namespace Platform::Windows

#endif // _WIN32
