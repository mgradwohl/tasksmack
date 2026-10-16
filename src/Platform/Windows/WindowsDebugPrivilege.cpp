#include "WindowsDebugPrivilege.h"

#include "WindowsElevation.h"
#include "WindowsHandles.h"

#include <spdlog/spdlog.h>

#include <optional>

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

    // The token opened above, not isProcessElevated()'s: AdjustTokenPrivileges below needs this one.
    const std::optional<bool> elevated = readTokenElevation(token.get(), fns.getTokenInformation);
    if (!elevated.has_value())
    {
        spdlog::warn("SeDebugPrivilege: reading TokenElevation failed (error {}); not enabled", GetLastError());
        return DebugPrivilegeResult::TokenUnavailable;
    }
    if (!*elevated)
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
