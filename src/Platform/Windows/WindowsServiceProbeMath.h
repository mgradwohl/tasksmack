#pragma once

// Pure mappings behind WindowsServiceProbe (#800): the Service Control Manager's numeric codes to
// Platform enums, and the svchost group from a service's command line. No Windows header, so the
// tests build on every platform (tests/Platform/WindowsMath/).

#include "Platform/IServiceProbe.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <utility>

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

/// The program a command line starts, and the rest of the line. The program is the first argument,
/// quoted ("C:\Program Files\x.exe" -a) or not, separated from the rest by spaces or tabs. An unquoted one may hold spaces (the SCM accepts
/// C:\Program Files\x.exe -a), so it runs to the first ".exe" that ends a word, or else to the
/// first space.
/// The characters that separate command-line arguments on Windows: a space or a tab.
inline constexpr std::string_view ARGUMENT_SEPARATORS = " \t";

[[nodiscard]] inline std::pair<std::string_view, std::string_view> splitProgram(std::string_view commandLine)
{
    const auto first = commandLine.find_first_not_of(ARGUMENT_SEPARATORS);
    if (first == std::string_view::npos)
    {
        return {};
    }
    commandLine.remove_prefix(first);
    if (commandLine.front() == '"')
    {
        const auto close = commandLine.find('"', 1);
        if (close == std::string_view::npos)
        {
            return {commandLine.substr(1), {}};
        }
        return {commandLine.substr(1, close - 1), commandLine.substr(close + 1)};
    }
    constexpr std::string_view EXE = ".exe";
    std::size_t end = commandLine.find_first_of(ARGUMENT_SEPARATORS);
    for (std::size_t i = 0; (i + EXE.size()) <= commandLine.size(); ++i)
    {
        const std::size_t after = i + EXE.size();
        const bool isExe = std::ranges::equal(
            commandLine.substr(i, EXE.size()), EXE, [](unsigned char a, unsigned char b) { return std::tolower(a) == b; });
        if (isExe && (after == commandLine.size() || ARGUMENT_SEPARATORS.contains(commandLine[after])))
        {
            end = after;
            break;
        }
    }
    if (end == std::string_view::npos)
    {
        return {commandLine, {}};
    }
    return {commandLine.substr(0, end), commandLine.substr(end)};
}

/// The svchost group a shared service runs in: the token after "-k" when the program is
/// svchost.exe (C:\WINDOWS\system32\svchost.exe -k netsvcs -p gives "netsvcs"). Empty for any other
/// program, including one whose name merely contains "svchost".
[[nodiscard]] inline std::string svchostGroup(std::string_view commandLine)
{
    const auto equalsIgnoringCase = [](std::string_view a, std::string_view b)
    {
        return std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    };

    const auto [program, args] = splitProgram(commandLine);
    const auto slash = program.find_last_of("\\/");
    const std::string_view basename = (slash == std::string_view::npos) ? program : program.substr(slash + 1);
    if (!equalsIgnoringCase(basename, "svchost.exe"))
    {
        return {};
    }
    bool afterFlag = false;
    std::string_view rest = args;
    for (auto start = rest.find_first_not_of(ARGUMENT_SEPARATORS); start != std::string_view::npos;
         start = rest.find_first_not_of(ARGUMENT_SEPARATORS))
    {
        rest.remove_prefix(start);
        const auto length = std::min(rest.find_first_of(ARGUMENT_SEPARATORS), rest.size());
        const std::string_view word = rest.substr(0, length);
        if (afterFlag)
        {
            return std::string(word);
        }
        afterFlag = equalsIgnoringCase(word, "-k");
        rest.remove_prefix(length);
    }
    return {};
}

/// How long a service's cached configuration is trusted before it is read again. Configuration
/// changes only when someone reconfigures the service, so this only bounds how stale it can look.
inline constexpr std::chrono::seconds CONFIG_REFRESH{30};

/// Whether a service's configuration should be read now: never attempted, or last attempted
/// (successfully or not) CONFIG_REFRESH or more ago. A denied read is retried no sooner than a
/// successful one, so a service the account can't query doesn't cost an open on every poll.
[[nodiscard]] constexpr bool
shouldRefreshConfig(std::chrono::steady_clock::time_point readAt, std::chrono::steady_clock::time_point now, bool attempted) noexcept
{
    return !attempted || (now - readAt) >= CONFIG_REFRESH;
}
/// The reason the UI shows when a Service Control Manager call fails with Win32 `error`:
/// ERROR_ACCESS_DENIED (5) as a denial, anything else as what could not be done ("be opened",
/// "list the services") and the code.
[[nodiscard]] inline std::string scmFailureReason(std::uint32_t error, std::string_view couldNot)
{
    constexpr std::uint32_t ACCESS_DENIED = 5;
    if (error == ACCESS_DENIED)
    {
        return "Access to the Service Control Manager was denied";
    }
    return std::format("The Service Control Manager could not {} (error {})", couldNot, error);
}

/// The probe's capabilities once OpenSCManagerW has been tried: everything when it succeeded
/// (`scmOpenError` 0), else nothing, with the reason the UI shows. ERROR_ACCESS_DENIED is 5.
[[nodiscard]] inline ServiceCapabilities capabilitiesForScmOpen(std::uint32_t scmOpenError)
{
    if (scmOpenError != 0)
    {
        ServiceCapabilities unavailable;
        unavailable.unavailableReason = scmFailureReason(scmOpenError, "be opened");
        return unavailable;
    }
    return {
        .canEnumerate = true,
        .hasDisplayName = true,
        .hasDescription = true,
        .hasStartType = true,
        .hasServiceType = true,
        .hasPid = true,
        .hasBinaryPath = true,
        .hasAccount = true,
        .hasGroup = true,
        .unavailableReason = {},
    };
}

} // namespace Platform::Windows::ServiceMath
