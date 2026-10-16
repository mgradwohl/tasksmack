#include "WindowsElevation.h"

#include "WindowsHandles.h"

#include <spdlog/spdlog.h>

#include <optional>

namespace Platform::Windows
{

std::optional<bool> readTokenElevation(HANDLE token, ElevationFunctions::GetTokenInformationFn getTokenInformation)
{
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    if (getTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) == FALSE)
    {
        return std::nullopt;
    }
    return elevation.TokenIsElevated != 0;
}

bool isProcessElevated(const ElevationFunctions& fns)
{
    UniqueHandle token;
    if (fns.openProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) == FALSE)
    {
        spdlog::debug("isProcessElevated: OpenProcessToken failed (error {}); reporting not elevated", GetLastError());
        return false;
    }
    const std::optional<bool> elevated = readTokenElevation(token.get(), fns.getTokenInformation);
    if (!elevated.has_value())
    {
        spdlog::debug("isProcessElevated: reading TokenElevation failed (error {}); reporting not elevated", GetLastError());
        return false;
    }
    return *elevated;
}

} // namespace Platform::Windows
