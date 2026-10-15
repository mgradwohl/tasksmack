#include "WindowsDebugPrivilege.h"

#include "WindowsHandles.h"

#include <spdlog/spdlog.h>

namespace Platform::Windows
{

DebugPrivilegeResult enableDebugPrivilegeIfElevated(const DebugPrivilegeFunctions& fns)
{
    UniqueHandle token;
    if (fns.openProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put()) == FALSE)
    {
        spdlog::warn("SeDebugPrivilege: OpenProcessToken failed (error {}); not enabled", GetLastError());
        return DebugPrivilegeResult::TokenUnavailable;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (fns.getTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) == FALSE)
    {
        spdlog::warn("SeDebugPrivilege: reading TokenElevation failed (error {}); not enabled", GetLastError());
        return DebugPrivilegeResult::TokenUnavailable;
    }
    if (elevation.TokenIsElevated == 0)
    {
        spdlog::debug("SeDebugPrivilege: not elevated; left disabled");
        return DebugPrivilegeResult::NotElevated;
    }

    LUID luid{};
    if (fns.lookupPrivilegeValue(nullptr, SE_DEBUG_NAME, &luid) == FALSE)
    {
        spdlog::warn("SeDebugPrivilege: LookupPrivilegeValueW failed (error {}); not enabled", GetLastError());
        return DebugPrivilegeResult::LookupFailed;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (fns.adjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr) == FALSE)
    {
        spdlog::warn("SeDebugPrivilege: AdjustTokenPrivileges failed (error {}); not enabled", GetLastError());
        return DebugPrivilegeResult::AdjustFailed;
    }
    // AdjustTokenPrivileges returns TRUE even when it enabled nothing; only the last error says so.
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        spdlog::warn("SeDebugPrivilege: the elevated token doesn't hold it (ERROR_NOT_ALL_ASSIGNED); not enabled");
        return DebugPrivilegeResult::NotAssigned;
    }

    spdlog::info("SeDebugPrivilege: enabled (elevated), so service processes can be acted on");
    return DebugPrivilegeResult::Enabled;
}

} // namespace Platform::Windows
