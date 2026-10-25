#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Platform
{

/// Where a startup entry is registered (#801). The Windows registry keys are under
/// Software\Microsoft\Windows\CurrentVersion; the folders are the Start menu's Startup folders; a packaged
/// app's startup task is declared in its package manifest (#1798). On Linux, an XDG autostart file (#2541).
enum class StartupLocation : std::uint8_t
{
    RunUser,             ///< HKCU ...\Run
    RunMachine,          ///< HKLM ...\Run (64-bit view)
    RunMachine32,        ///< HKLM ...\Run, 32-bit view (WOW6432Node)
    RunOnceUser,         ///< HKCU ...\RunOnce
    RunOnceMachine,      ///< HKLM ...\RunOnce
    StartupFolderUser,   ///< The user's Startup folder
    StartupFolderCommon, ///< The all-users Startup folder
    PackagedApp,         ///< A packaged (MSIX / Store) app's windows.startupTask, for the current user
    XdgAutostartUser,    ///< Linux: a file in ~/.config/autostart only (#2541)
    XdgAutostartSystem,  ///< Linux: a file in /etc/xdg/autostart (or another $XDG_CONFIG_DIRS), which the
                         ///< user's copy of the same name, when there is one, switches
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
    /// The name shown (#1873): for a Run or Startup folder entry, programName (as Settings > Apps >
    /// Startup names them); for a RunOnce entry, registrationName, its registry value name (#1971);
    /// for a packaged app's startup task, its display name from the manifest.
    std::string name;
    /// The program's own name (#1971), for a Run, RunOnce or Startup folder entry: its version-resource
    /// FileDescription, else the program's file name, else registrationName. Equals name except for a
    /// RunOnce entry, whose tooltip shows it. Empty for a packaged app's startup task.
    std::string programName;
    /// The name the entry is registered under, which identifies it (#1873): the Run or RunOnce value
    /// name, the Startup folder file's name with its extension ("Tool.lnk"), or a packaged startup task's
    /// TaskId. May equal name.
    std::string registrationName;
    /// For a packaged app's startup task, its package family name ("Contoso.Chat_8wekyb3d8bbwe"), which
    /// with registrationName (the TaskId) identifies the task. Empty for any other entry.
    std::string packageFamily;
    std::string command;        ///< The command line it runs (a shortcut's target and arguments).
    std::string executablePath; ///< The program the command runs, environment variables expanded.
    std::string sourcePath;     ///< For a Startup folder entry, the file in the folder (e.g. the .lnk).
    /// The image a packaged app is shown with (#1800): its manifest's logo, as the manifest names it
    /// under the package's folder ("...\Assets\Square44x44Logo.png"). The package holds it as
    /// scale and target-size variants beside that name, so the file itself may not exist. Empty for
    /// any other entry: its icon is executablePath's.
    std::string iconPath;
    StartupLocation location = StartupLocation::RunUser;
    StartupScope scope = StartupScope::User;
    bool enabled = true; ///< From StartupApproved (entries it doesn't list are enabled), or a packaged task's state.
    /// Whether an administrator or group policy sets the enabled state (a packaged task's
    /// EnabledByPolicy or DisabledByPolicy), so the user can't change it.
    bool setByPolicy = false;
    /// Why its enabled state can't be changed here, when the platform knows a reason of its own (Linux: an
    /// autostart entry for another desktop, "Starts only in KDE"). Empty otherwise.
    std::string notSwitchableReason;
    std::uint64_t disabledAtUnixSeconds = 0; ///< When it was disabled (Unix seconds); 0 when unknown or enabled.
    std::string publisher;                   ///< The executable's version-resource CompanyName, or the package's publisher.
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

/// The probe for a platform without a startup implementation (and the synthetic runs' stand-in): no
/// entries, and canEnumerate false so the UI can say so.
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
