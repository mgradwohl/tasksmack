#pragma once

// Pure mappings behind WindowsServiceProbe (#800): the Service Control Manager's numeric codes to
// Platform enums, and the svchost group from a service's command line. No Windows header, so the
// tests build on every platform (tests/Platform/WindowsMath/).

#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace Platform::Windows::ServiceMath
{

/// SERVICE_STATUS_PROCESS::dwCurrentState (SERVICE_STOPPED = 1 ... SERVICE_PAUSED = 7).
[[nodiscard]] constexpr ServiceState stateFromCode(std::uint32_t code) noexcept
{
    switch (code)
    {
    case 1:
        return ServiceState::Stopped;
    case 2:
        return ServiceState::StartPending;
    case 3:
        return ServiceState::StopPending;
    case 4:
        return ServiceState::Running;
    case 5:
        return ServiceState::ContinuePending;
    case 6:
        return ServiceState::PausePending;
    case 7:
        return ServiceState::Paused;
    default:
        return ServiceState::Unknown;
    }
}

/// QUERY_SERVICE_CONFIGW::dwStartType (SERVICE_BOOT_START = 0 ... SERVICE_DISABLED = 4), with
/// SERVICE_DELAYED_AUTO_START_INFO's flag: delayed applies only to an automatic service.
[[nodiscard]] constexpr ServiceStartType startTypeFromCode(std::uint32_t code, bool delayedAutoStart) noexcept
{
    switch (code)
    {
    case 0:
        return ServiceStartType::Boot;
    case 1:
        return ServiceStartType::System;
    case 2:
        return delayedAutoStart ? ServiceStartType::AutomaticDelayed : ServiceStartType::Automatic;
    case 3:
        return ServiceStartType::Manual;
    case 4:
        return ServiceStartType::Disabled;
    default:
        return ServiceStartType::Unknown;
    }
}

/// dwServiceType as a short label. User-service templates (SERVICE_USER_SERVICE, 0x40) and their
/// per-user instances (0x80) are labelled as such before the own/shared process bits.
[[nodiscard]] inline std::string serviceTypeText(std::uint32_t type)
{
    constexpr std::uint32_t KERNEL_DRIVER = 0x1;
    constexpr std::uint32_t FILE_SYSTEM_DRIVER = 0x2;
    constexpr std::uint32_t OWN_PROCESS = 0x10;
    constexpr std::uint32_t SHARE_PROCESS = 0x20;
    constexpr std::uint32_t USER_SERVICE = 0x40;
    constexpr std::uint32_t USER_INSTANCE = 0x80;

    std::string text;
    if ((type & SHARE_PROCESS) != 0)
    {
        text = "Shared process";
    }
    else if ((type & OWN_PROCESS) != 0)
    {
        text = "Own process";
    }
    else if ((type & (KERNEL_DRIVER | FILE_SYSTEM_DRIVER)) != 0)
    {
        text = "Driver";
    }
    else
    {
        text = "Other";
    }
    if ((type & USER_INSTANCE) != 0)
    {
        text += " (user instance)";
    }
    else if ((type & USER_SERVICE) != 0)
    {
        text += " (user template)";
    }
    return text;
}

/// The svchost group a shared service runs in: the token after "-k" in an svchost.exe command line
/// ("...\svchost.exe -k netsvcs -p" gives "netsvcs"). Empty for any other command line.
[[nodiscard]] inline std::string svchostGroup(std::string_view commandLine)
{
    std::string lower(commandLine);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!lower.contains("svchost.exe"))
    {
        return {};
    }
    const auto flag = lower.find(" -k ");
    if (flag == std::string::npos)
    {
        return {};
    }
    const auto start = commandLine.find_first_not_of(' ', flag + 4);
    if (start == std::string_view::npos)
    {
        return {};
    }
    const auto end = commandLine.find(' ', start);
    return std::string(commandLine.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

} // namespace Platform::Windows::ServiceMath
