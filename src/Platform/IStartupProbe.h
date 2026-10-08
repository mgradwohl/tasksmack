#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// Where a startup entry is registered (#801). The Windows registry keys are under
/// Software\Microsoft\Windows\CurrentVersion; the folders are the Start menu's Startup folders.
enum class StartupLocation : std::uint8_t
{
    RunUser,             ///< HKCU ...\Run
    RunMachine,          ///< HKLM ...\Run (64-bit view)
    RunMachine32,        ///< HKLM ...\Run, 32-bit view (WOW6432Node)
    RunOnceUser,         ///< HKCU ...\RunOnce
    RunOnceMachine,      ///< HKLM ...\RunOnce
    StartupFolderUser,   ///< The user's Startup folder
    StartupFolderCommon, ///< The all-users Startup folder
};

/// Whether an entry starts for the current user only or for every user of the machine.
enum class StartupScope : std::uint8_t
{
    User,
    Machine,
};

/// Whether the program an entry starts was found.
enum class StartupTargetState : std::uint8_t
{
    Unresolved, ///< The executable could not be determined (e.g. a shortcut whose target was not read).
    Present,
    Missing, ///< The resolved executable does not exist.
};

/// One startup entry, raw as the OS registers it. A field the probe could not read (its capability
/// is false, or this entry's file could not be read) is left empty, 0 or Unresolved.
struct StartupEntry
{
    std::string name;           ///< The registry value name, or the Startup folder file name.
    std::string command;        ///< The command line it runs (a shortcut's target and arguments).
    std::string executablePath; ///< The program the command runs, environment variables expanded.
    std::string sourcePath;     ///< For a Startup folder entry, the file in the folder (e.g. the .lnk).
    StartupLocation location = StartupLocation::RunUser;
    StartupScope scope = StartupScope::User;
    bool enabled = true;                     ///< From StartupApproved; entries it doesn't list are enabled.
    std::uint64_t disabledAtUnixSeconds = 0; ///< When it was disabled (Unix seconds); 0 when unknown or enabled.
    std::string publisher;                   ///< The executable's version-resource CompanyName.
    StartupTargetState target = StartupTargetState::Unresolved;
};

/// Which StartupEntry fields this platform can fill at all, following ServiceCapabilities.
struct StartupCapabilities
{
    bool canEnumerate = false;        ///< False: no startup list here; unavailableReason says why.
    bool hasEnabledState = false;     ///< enabled reflects the OS's enabled/disabled state.
    bool hasDisabledTime = false;     ///< disabledAtUnixSeconds can be set.
    bool hasPublisher = false;        ///< publisher can be set.
    bool canResolveShortcuts = false; ///< Startup folder shortcuts' targets can be read.
    /// Why canEnumerate is false, for the UI to show. Empty when canEnumerate is true.
    std::string unavailableReason;
};

/// Reads the programs registered to start at sign-in (read-only; #801 phase 1). Called from one
/// thread at a time, which may differ between calls.
class IStartupProbe
{
  public:
    virtual ~IStartupProbe() = default;

    IStartupProbe() = default;
    IStartupProbe(const IStartupProbe&) = default;
    IStartupProbe& operator=(const IStartupProbe&) = default;
    IStartupProbe(IStartupProbe&&) = default;
    IStartupProbe& operator=(IStartupProbe&&) = default;

    [[nodiscard]] virtual StartupCapabilities capabilities() const = 0;

    /// Every startup entry, from every location that could be read. A location that can't be read
    /// (missing key, denied) contributes nothing; the others are still listed.
    [[nodiscard]] virtual std::vector<StartupEntry> enumerate() = 0;
};

/// The probe for a platform without a startup implementation yet (Linux until XDG autostart support
/// lands): no entries, and canEnumerate false so the UI can say so.
class UnsupportedStartupProbe final : public IStartupProbe
{
  public:
    [[nodiscard]] StartupCapabilities capabilities() const override
    {
        return {.unavailableReason = "Startup apps aren't available on this platform yet"};
    }

    [[nodiscard]] std::vector<StartupEntry> enumerate() override
    {
        return {};
    }
};

} // namespace Platform
