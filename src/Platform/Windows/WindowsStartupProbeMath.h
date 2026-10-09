#pragma once

// Pure helpers behind WindowsStartupProbe and WindowsStartupActions (#801): the StartupApproved value
// format (parse and encode), its keys, FILETIME to Unix time, and the executable a Run command line
// starts. No Windows header, so the tests build on every platform (tests/Platform/WindowsMath/).

#include "Platform/IStartupProbe.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

/// The StartupApproved keys (under HKCU or HKLM) that record the Run, 32-bit Run and Startup folder
/// entries' state.
inline constexpr const wchar_t* APPROVED_RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
inline constexpr const wchar_t* APPROVED_RUN32_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32";
inline constexpr const wchar_t* APPROVED_FOLDER_KEY =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder";

/// Where one entry's StartupApproved value lives: the root (HKLM when `machine`) and the subkey.
struct ApprovedKey
{
    bool machine = false;
    const wchar_t* subkey = nullptr;

    friend constexpr bool operator==(const ApprovedKey&, const ApprovedKey&) noexcept = default;
};

/// The StartupApproved key for an entry at @p location; nullopt for RunOnce, which has none.
[[nodiscard]] constexpr std::optional<ApprovedKey> approvedKeyFor(StartupLocation location) noexcept
{
    switch (location)
    {
    case StartupLocation::RunUser:
        return ApprovedKey{.machine = false, .subkey = APPROVED_RUN_KEY};
    case StartupLocation::RunMachine:
        return ApprovedKey{.machine = true, .subkey = APPROVED_RUN_KEY};
    case StartupLocation::RunMachine32:
        return ApprovedKey{.machine = true, .subkey = APPROVED_RUN32_KEY};
    case StartupLocation::StartupFolderUser:
        return ApprovedKey{.machine = false, .subkey = APPROVED_FOLDER_KEY};
    case StartupLocation::StartupFolderCommon:
        return ApprovedKey{.machine = true, .subkey = APPROVED_FOLDER_KEY};
    case StartupLocation::RunOnceUser:
    case StartupLocation::RunOnceMachine:
        break;
    }
    return std::nullopt;
}

/// The StartupApproved value name for @p entry: a Run entry's value name, or a Startup folder entry's
/// file name with its extension ("Tool.lnk"), as the probe matches them.
[[nodiscard]] inline std::string approvedValueName(const StartupEntry& entry)
{
    if (entry.location != StartupLocation::StartupFolderUser && entry.location != StartupLocation::StartupFolderCommon)
    {
        return entry.name;
    }
    const auto slash = entry.sourcePath.find_last_of("\\/");
    return (slash == std::string::npos) ? entry.sourcePath : entry.sourcePath.substr(slash + 1);
}

/// The StartupApproved value that records @p enabled, built on @p existing (the current value; empty
/// when there is none). Byte 0 gets bit 0 cleared when enabled and set when disabled, with bit 1 set
/// (0x02 / 0x03, as Task Manager writes); its other bits, bytes 1-3 and anything past byte 11 are kept.
/// Bytes 4-11 become @p disabledAtFileTime little-endian when disabling, zero when enabling. A value
/// shorter than 12 bytes is padded with zeros to the standard 12.
[[nodiscard]] inline std::vector<std::uint8_t>
encodeStartupApproved(std::span<const std::uint8_t> existing, bool enabled, std::uint64_t disabledAtFileTime)
{
    constexpr std::size_t STANDARD_SIZE = 12;
    constexpr std::size_t TIME_OFFSET = 4;
    constexpr std::size_t TIME_BYTES = 8;
    std::vector<std::uint8_t> value(existing.begin(), existing.end());
    if (value.size() < STANDARD_SIZE)
    {
        value.resize(STANDARD_SIZE, 0);
    }
    value[0] = static_cast<std::uint8_t>((value[0] & 0xFEU) | 0x02U | (enabled ? 0U : 1U));
    const std::uint64_t time = enabled ? 0 : disabledAtFileTime;
    for (std::size_t i = 0; i < TIME_BYTES; ++i)
    {
        value[TIME_OFFSET + i] = static_cast<std::uint8_t>(time >> (8U * i));
    }
    return value;
}

/// A Win32 error from a StartupApproved write, in words the result line shows as they stand.
[[nodiscard]] inline std::string approvedErrorText(std::uint32_t code)
{
    if (code == 5) // ERROR_ACCESS_DENIED: an all-users entry, not elevated
    {
        return "Requires administrator";
    }
    return std::format("Windows error {}", code);
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
