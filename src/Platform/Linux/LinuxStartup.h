#pragma once

// The Startup tab on Linux (#2541): the XDG autostart entries (XdgAutostart.h), listed and switched on or
// off the XDG way, by a user copy in ~/.config/autostart that hides or shows the entry. System files are
// never written, so no entry needs root, whoever it starts for.

#include "Platform/IStartupActions.h"
#include "Platform/IStartupProbe.h"
#include "Platform/Linux/XdgAutostart.h"

#include <string>
#include <vector>

namespace Platform
{

/// Lists the autostart entries: one per file name, the user's directory's file winning over a system
/// one, as the session itself picks them.
class LinuxStartupProbe final : public IStartupProbe
{
  public:
    /// From the environment ($HOME, $XDG_CONFIG_HOME, $XDG_CONFIG_DIRS, $XDG_CURRENT_DESKTOP), read here.
    LinuxStartupProbe();
    /// Test seam: these directories, @p currentDesktops for OnlyShowIn and NotShowIn, and @p searchPath
    /// (a $PATH) to find an Exec's program on.
    LinuxStartupProbe(XdgAutostart::Dirs dirs, std::string currentDesktops, std::string searchPath);

    [[nodiscard]] StartupCapabilities capabilities() const override;
    [[nodiscard]] std::vector<StartupEntry> enumerate() override;

  private:
    XdgAutostart::Dirs m_Dirs;
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
    LinuxStartupActions();
    /// Test seam: these directories.
    explicit LinuxStartupActions(XdgAutostart::Dirs dirs);

    [[nodiscard]] StartupActionCapabilities capabilities() const override;
    [[nodiscard]] StartupActionResult setEnabled(const StartupEntry& entry, bool enabled) override;

  private:
    XdgAutostart::Dirs m_Dirs;
};

} // namespace Platform
