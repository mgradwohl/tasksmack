#pragma once

// Pure helpers behind WindowsStartupProbe and WindowsStartupActions (#801): the StartupApproved value
// format (parse and encode), its keys, and the executable a Run command line starts (FILETIME to Unix
// time is in WindowsConversions.h). No Windows header, so the tests build on every platform
// (tests/Platform/WindowsMath/).

#include "Platform/AsciiCase.h"
#include "Platform/IStartupProbe.h"
#include "WindowsConversions.h"

#include <array>
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

/// The StartupApproved key for an entry at @p location; nullopt for RunOnce, which has none, and for a
/// packaged app's startup task, whose state the app model keeps.
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
    case StartupLocation::PackagedApp:
    case StartupLocation::XdgAutostartUser: // Linux's (#2541)
    case StartupLocation::XdgAutostartSystem:
        break;
    }
    return std::nullopt;
}

/// The StartupApproved value name for @p entry: a Run entry's value name (its registrationName; #1873),
/// or a Startup folder entry's file name with its extension ("Tool.lnk"), as the probe matches them.
[[nodiscard]] inline std::string approvedValueName(const StartupEntry& entry)
{
    if (entry.location != StartupLocation::StartupFolderUser && entry.location != StartupLocation::StartupFolderCommon)
    {
        return entry.registrationName;
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
    case StartupLocation::PackagedApp:
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
            if (Platform::equalsIgnoreCase(commandLine.substr(pos, extension.size()), extension))
            {
                return std::string(commandLine.substr(0, pos + extension.size()));
            }
        }
    }
    return std::string(commandLine.substr(0, commandLine.find_first_of(" \t")));
}

/// Where packaged apps' startup tasks keep their state (#1798), under HKCU: one subkey per package
/// family, and under it one per startup task, named by its TaskId, holding State and LastDisabledTime.
inline constexpr const wchar_t* PACKAGED_TASKS_KEY =
    L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\SystemAppData";

/// Whether a startup task's State (Windows.ApplicationModel.StartupTaskState) starts the app: Enabled
/// (2) or EnabledByPolicy (4). Disabled (0), DisabledByUser (1), DisabledByPolicy (3) and anything
/// unknown don't.
[[nodiscard]] constexpr bool packagedTaskEnabled(std::uint32_t state) noexcept
{
    return state == 2 || state == 4;
}

/// The StartupTaskState values TaskSmack writes to a packaged startup task's State (#1873), as
/// Settings > Apps > Startup does: Enabled when the user turns it on, DisabledByUser when off.
inline constexpr std::uint32_t PACKAGED_TASK_DISABLED_BY_USER = 1;
inline constexpr std::uint32_t PACKAGED_TASK_ENABLED = 2;

/// Whether a startup task's State is set by an administrator or group policy: DisabledByPolicy (3) or
/// EnabledByPolicy (4). The user can't change such a task.
[[nodiscard]] constexpr bool packagedTaskSetByPolicy(std::uint32_t state) noexcept
{
    return state == 3 || state == 4;
}

/// The name a Run, RunOnce or Startup folder entry is shown by (#1873), as Settings > Apps > Startup
/// names it: its program's @p fileDescription (version resource), else the file name in
/// @p executablePath ("msedge.exe"), else @p registrationName. Spaces around the description are dropped.
[[nodiscard]] inline std::string
startupDisplayName(std::string_view fileDescription, std::string_view executablePath, std::string_view registrationName)
{
    if (const auto first = fileDescription.find_first_not_of(' '); first != std::string_view::npos)
    {
        const auto last = fileDescription.find_last_not_of(' ');
        return std::string(fileDescription.substr(first, last - first + 1));
    }
    const auto slash = executablePath.find_last_of("\\/");
    const std::string_view fileName = (slash == std::string_view::npos) ? executablePath : executablePath.substr(slash + 1);
    return std::string(fileName.empty() ? registrationName : fileName);
}

/// The Name column's text for an entry at @p location (#1971): a RunOnce entry (HKCU or HKLM) is
/// shown by its @p registrationName, the registry value name, since RunOnce clean-up commands often
/// share one program (cmd.exe) and neither Task Manager nor Settings lists them under any other name;
/// every other entry by @p programName (startupDisplayName's result, or a packaged app's manifest
/// name). A RunOnce entry with no value name falls back to @p programName.
[[nodiscard]] inline std::string startupEntryName(StartupLocation location, std::string_view programName, std::string_view registrationName)
{
    switch (location)
    {
    case StartupLocation::RunOnceUser:
    case StartupLocation::RunOnceMachine:
        if (!registrationName.empty())
        {
            return std::string(registrationName);
        }
        break;
    case StartupLocation::RunUser:
    case StartupLocation::RunMachine:
    case StartupLocation::RunMachine32:
    case StartupLocation::StartupFolderUser:
    case StartupLocation::StartupFolderCommon:
    case StartupLocation::PackagedApp:
    case StartupLocation::XdgAutostartUser: // Linux's (#2541)
    case StartupLocation::XdgAutostartSystem:
        break;
    }
    return std::string(programName);
}

/// The package name in a package family name: "Microsoft.WindowsTerminal_8wekyb3d8bbwe" gives
/// "Microsoft.WindowsTerminal". A name without the "_publisherId" suffix is returned whole.
[[nodiscard]] constexpr std::string_view packageNameOfFamily(std::string_view family) noexcept
{
    return family.substr(0, family.rfind('_'));
}

/// The ms-resource URI a manifest string refers to, to be resolved in the package's resources; empty
/// when @p value is plain text. "ms-resource:AppName" names a string in the package's Resources map
/// ("ms-resource://Package/Resources/AppName"); "ms-resource:Map/Name" a string in another of its maps;
/// "ms-resource:/Map/Name" and "ms-resource://Package/Map/Name" are already rooted.
[[nodiscard]] inline std::string resourceUri(std::string_view value, std::string_view packageName)
{
    constexpr std::string_view SCHEME = "ms-resource:";
    if (value.size() < SCHEME.size())
    {
        return {};
    }
    if (!Platform::equalsIgnoreCase(value.substr(0, SCHEME.size()), SCHEME))
    {
        return {};
    }
    const std::string_view rest = value.substr(SCHEME.size());
    if (rest.empty())
    {
        return {};
    }
    if (rest.starts_with("//"))
    {
        return std::format("{}{}", SCHEME, rest);
    }
    if (rest.starts_with('/'))
    {
        return std::format("{}//{}{}", SCHEME, packageName, rest);
    }
    if (rest.contains('/'))
    {
        return std::format("{}//{}/{}", SCHEME, packageName, rest);
    }
    return std::format("{}//{}/Resources/{}", SCHEME, packageName, rest);
}

} // namespace Platform::Windows::StartupMath
