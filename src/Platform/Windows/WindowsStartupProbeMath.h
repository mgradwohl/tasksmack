#pragma once

// Pure helpers behind WindowsStartupProbe (#801): the StartupApproved value format, FILETIME to Unix
// time, and the executable a Run command line starts. No Windows header, so the tests build on every
// platform (tests/Platform/WindowsMath/).

#include "Platform/IStartupProbe.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Platform::Windows::StartupMath
{

/// What one StartupApproved value says about its entry.
struct ApprovedState
{
    bool enabled = true;
    std::uint64_t disabledAtUnixSeconds = 0; ///< 0 when enabled, or when no time is recorded.

    friend constexpr bool operator==(const ApprovedState&, const ApprovedState&) noexcept = default;
};

/// 100 ns FILETIME ticks between 1601-01-01 and the Unix epoch.
inline constexpr std::uint64_t FILETIME_UNIX_EPOCH = 116444736000000000ULL;
inline constexpr std::uint64_t FILETIME_TICKS_PER_SECOND = 10000000ULL;

/// A FILETIME (100 ns ticks since 1601) as Unix seconds; 0 for one at or before the Unix epoch.
[[nodiscard]] constexpr std::uint64_t fileTimeToUnixSeconds(std::uint64_t fileTime) noexcept
{
    return fileTime > FILETIME_UNIX_EPOCH ? (fileTime - FILETIME_UNIX_EPOCH) / FILETIME_TICKS_PER_SECOND : 0;
}

/// Parses a REG_BINARY value under Explorer\StartupApproved\{Run,Run32,StartupFolder} (normally 12
/// bytes). Byte 0 is even when the entry is enabled (0x02, 0x06) and odd when it is disabled (0x03,
/// 0x07); bytes 4-11 are the little-endian FILETIME it was disabled at (zero when enabled).
/// An empty value is nullopt (treated as no record: enabled). A value too short for the FILETIME
/// still gives the enabled state, with no time.
[[nodiscard]] constexpr std::optional<ApprovedState> parseStartupApproved(std::span<const std::uint8_t> data) noexcept
{
    if (data.empty())
    {
        return std::nullopt;
    }
    ApprovedState state;
    state.enabled = (data[0] & 1U) == 0;
    constexpr std::size_t TIME_OFFSET = 4;
    constexpr std::size_t TIME_BYTES = 8;
    if (!state.enabled && data.size() >= TIME_OFFSET + TIME_BYTES)
    {
        std::uint64_t fileTime = 0;
        for (std::size_t i = 0; i < TIME_BYTES; ++i)
        {
            fileTime |= static_cast<std::uint64_t>(data[TIME_OFFSET + i]) << (8U * i);
        }
        state.disabledAtUnixSeconds = fileTimeToUnixSeconds(fileTime);
    }
    return state;
}

/// Whether the location is per user or machine-wide.
[[nodiscard]] constexpr StartupScope scopeOf(StartupLocation location) noexcept
{
    switch (location)
    {
    case StartupLocation::RunUser:
    case StartupLocation::RunOnceUser:
    case StartupLocation::StartupFolderUser:
        return StartupScope::User;
    case StartupLocation::RunMachine:
    case StartupLocation::RunMachine32:
    case StartupLocation::RunOnceMachine:
    case StartupLocation::StartupFolderCommon:
    default:
        return StartupScope::Machine;
    }
}

/// The program a Run command line starts: the quoted first token (`"C:\Program Files\A\a.exe" -x`),
/// else an unquoted path up to the first ".exe", ".com", ".bat" or ".cmd" that ends a word (Windows
/// itself accepts `C:\Program Files\A\a.exe -x` unquoted), else the first space-separated token.
/// Environment variables are left as they are; the probe expands them first.
[[nodiscard]] inline std::string executableFromCommandLine(std::string_view commandLine)
{
    const auto first = commandLine.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return {};
    }
    commandLine.remove_prefix(first);
    if (commandLine.front() == '"')
    {
        commandLine.remove_prefix(1);
        const auto close = commandLine.find('"');
        return std::string(commandLine.substr(0, close));
    }

    constexpr std::array<std::string_view, 4> EXTENSIONS = {".exe", ".com", ".bat", ".cmd"};
    const auto endsWord = [&](std::size_t end)
    {
        return end == commandLine.size() || commandLine[end] == ' ' || commandLine[end] == '\t';
    };
    for (std::size_t pos = 0; pos < commandLine.size(); ++pos)
    {
        if (commandLine[pos] != '.')
        {
            continue;
        }
        for (const std::string_view extension : EXTENSIONS)
        {
            if (pos + extension.size() > commandLine.size() || !endsWord(pos + extension.size()))
            {
                continue;
            }
            bool matches = true;
            for (std::size_t i = 0; i < extension.size(); ++i)
            {
                matches = matches && std::tolower(static_cast<unsigned char>(commandLine[pos + i])) == extension[i];
            }
            if (matches)
            {
                return std::string(commandLine.substr(0, pos + extension.size()));
            }
        }
    }
    return std::string(commandLine.substr(0, commandLine.find_first_of(" \t")));
}

} // namespace Platform::Windows::StartupMath
