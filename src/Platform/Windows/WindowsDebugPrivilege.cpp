#include "WindowsDebugPrivilege.h"

#include "WindowsElevation.h"
#include "WindowsHandles.h"

#include <spdlog/spdlog.h>

#include <optional>

namespace Platform::Windows
{

namespace
{

/// Enables @p privilege (@p label in the log) on TaskSmack's own token when it is elevated.
DebugPrivilegeResult enablePrivilegeIfElevated(const DebugPrivilegeFunctions& fns, LPCWSTR privilege, const char* label)
{
    UniqueHandle token;
    if (fns.openProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, token.put()) == FALSE)
    {
        spdlog::warn("{}: OpenProcessToken failed (error {}); not enabled", label, GetLastError());
        return DebugPrivilegeResult::TokenUnavailable;
    }

    // The token opened above, not isProcessElevated()'s: AdjustTokenPrivileges below needs this one.
    const std::optional<bool> elevated = readTokenElevation(token.get(), fns.getTokenInformation);
    if (!elevated.has_value())
    {
        spdlog::warn("{}: reading TokenElevation failed (error {}); not enabled", label, GetLastError());
        return DebugPrivilegeResult::TokenUnavailable;
    }
    if (!*elevated)
    {
        spdlog::debug("{}: not elevated; left disabled", label);
        return DebugPrivilegeResult::NotElevated;
    }

    LUID luid{};
    if (fns.lookupPrivilegeValue(nullptr, privilege, &luid) == FALSE)
    {
        spdlog::warn("{}: LookupPrivilegeValueW failed (error {}); not enabled", label, GetLastError());
        return DebugPrivilegeResult::LookupFailed;
    }

    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (fns.adjustTokenPrivileges(token.get(), FALSE, &privileges, sizeof(privileges), nullptr, nullptr) == FALSE)
    {
        spdlog::warn("{}: AdjustTokenPrivileges failed (error {}); not enabled", label, GetLastError());
        return DebugPrivilegeResult::AdjustFailed;
    }
    // AdjustTokenPrivileges returns TRUE even when it enabled nothing; only the last error says so.
    if (GetLastError() == ERROR_NOT_ALL_ASSIGNED)
    {
        spdlog::warn("{}: the elevated token doesn't hold it (ERROR_NOT_ALL_ASSIGNED); not enabled", label);
        return DebugPrivilegeResult::NotAssigned;
    }

    spdlog::info("{}: enabled (elevated)", label);
    return DebugPrivilegeResult::Enabled;
}

} // namespace

DebugPrivilegeResult enableDebugPrivilegeIfElevated(const DebugPrivilegeFunctions& fns)
{
    // So service processes can be acted on (#2128).
    return enablePrivilegeIfElevated(fns, SE_DEBUG_NAME, "SeDebugPrivilege");
}

DebugPrivilegeResult enableIncreaseBasePriorityPrivilegeIfElevated(const DebugPrivilegeFunctions& fns)
{
    // High I/O priority (#3362): NtSetInformationProcess checks the privilege, and doesn't enable it for
    // the call as SetPriorityClass does for Realtime.
    return enablePrivilegeIfElevated(fns, SE_INC_BASE_PRIORITY_NAME, "SeIncreaseBasePriorityPrivilege");
}

} // namespace Platform::Windows
