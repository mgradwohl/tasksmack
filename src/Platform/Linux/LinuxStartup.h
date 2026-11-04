#pragma once

// The Startup tab on Linux (#2541): the XDG autostart entries (XdgAutostart.h), listed and switched on or
// off the XDG way, by a user copy in ~/.config/autostart that hides or shows the entry. System files are
// never written, so no entry needs root, whoever it starts for.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Linux/XdgAutostart.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

/// Lists the autostart entries: one per file name, the user's directory's file winning over a system
/// one, as the session itself picks them.
/// Where the systemd user units the Startup tab lists are (#3082).
struct SystemdUserDirs
{
    std::filesystem::path user;                  ///< ~/.config/systemd/user; empty to skip the user's units
    std::vector<std::filesystem::path> system;   ///< /etc/systemd/user: units enabled for every user
    std::vector<std::filesystem::path> unitPath; ///< Where a timer's service is looked for, in order
};

/// The directories for this user: $XDG_CONFIG_HOME (or ~/.config)/systemd/user, /etc/systemd/user, and the
/// packages' /usr/local/lib/systemd/user and /usr/lib/systemd/user for the unit files themselves.
[[nodiscard]] SystemdUserDirs systemdUserDirs(std::string_view home, std::string_view xdgConfigHome);

/// Where the cron jobs the Startup tab lists come from (#3425), read-only.
struct CronSources
{
    /// The user's own crontab as `crontab -l` prints it: nullopt when there is none or it can't be read. Unset to skip.
    std::function<std::optional<std::string>()> userCrontab;
    /// The system tables, read as files: /etc/crontab and each file in /etc/cron.d (they name the user each job
    /// runs as).
    std::vector<std::filesystem::path> systemTables;
};

/// This machine's: `crontab -l`, re-run at most every Domain::Sampling::STARTUP_CRON_REFRESH_SECONDS, and
/// /etc/crontab plus the files in /etc/cron.d, listed when enumerate() runs.
[[nodiscard]] CronSources cronSources();

class LinuxStartupProbe final : public IStartupProbe
{
  public:
    /// From the environment ($HOME, $XDG_CONFIG_HOME, $XDG_CONFIG_DIRS, $XDG_CURRENT_DESKTOP), read here.
    LinuxStartupProbe();
    /// Test seam: these directories, @p currentDesktops for OnlyShowIn and NotShowIn, and @p searchPath
    /// (a $PATH) to find an Exec's program on.
    LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath);
    /// Test seam with @p systemd's directories too (#3082); the three-argument form lists no systemd units.
    LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath, SystemdUserDirs systemd);
    /// Test seam with @p cron's sources too (#3425); the forms above list no cron jobs.
    LinuxStartupProbe(
        XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath, SystemdUserDirs systemd, CronSources cron);

    [[nodiscard]] StartupCapabilities capabilities() const override;
    [[nodiscard]] std::vector<StartupEntry> enumerate() override;

  private:
    /// The systemd user units enabled to start with the session or on a timer (#3082), read-only.
    void addSystemdUnits(std::vector<StartupEntry>& entries) const;
    /// The cron jobs (#3425): the user's crontab, then the system tables, read-only.
    void addCronJobs(std::vector<StartupEntry>& entries) const;

    XdgAutostart::Dirs m_Dirs;
    SystemdUserDirs m_Systemd;
    CronSources m_Cron;
    std::string m_CurrentDesktops;
    std::string m_SearchPath; ///< $PATH, to find an Exec's program
};

/// Switches an entry by writing its user file: the entry's own text, or a copy of the system file it
/// overrides, with Hidden (and X-GNOME-Autostart-enabled, when present) set. Switching back on a copy that
/// differs from its system file in nothing else removes the copy, so later updates of the system file
/// apply again.
class LinuxStartupActions final : public IStartupActions
{
  public:
    /// Switches a systemd user unit (#3425): SystemdBus::setUserUnitEnabled(), or a test's fake.
    using UserUnitSwitch = std::function<StartupActionResult(std::string_view unit, bool enabled)>;

    LinuxStartupActions();
    /// Test seam: these directories, switching user units through the real user manager.
    explicit LinuxStartupActions(XdgAutostart::Dirs dirs);
    /// Test seam: these directories, and @p switchUserUnit for systemd user units.
    LinuxStartupActions(XdgAutostart::Dirs dirs, UserUnitSwitch switchUserUnit);

    [[nodiscard]] StartupActionCapabilities capabilities() const override;
    [[nodiscard]] StartupActionResult setEnabled(const StartupEntry& entry, bool enabled) override;

  private:
    XdgAutostart::Dirs m_Dirs;
    UserUnitSwitch m_SwitchUserUnit;
};

} // namespace Platform
