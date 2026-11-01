#pragma once

// The sign-in entry that starts TaskSmack when the user signs in (#3093, part 2): raw reads and writes,
// no decisions (those are Domain/StartAtLogin.h's). Windows: the per-user Run key,
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "TaskSmack" (WindowsStartAtLogin); no elevation.
// Linux: a per-user XDG autostart entry, ~/.config/autostart/tasksmack.desktop (LinuxStartAtLogin, #3254).
//
// Blocking (registry or file calls): used on a worker (App::StartAtLoginSetting), never on the UI thread (#2731).

#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace Platform
{

/// What read() found.
struct StartAtLoginFacts
{
    std::optional<std::string> command; ///< The entry's command line (UTF-8), or nullopt when there is none
    std::string executable;             ///< The running TaskSmack's full path (UTF-8), which the entry should start
    bool installed = false;             ///< @p executable is under Program Files: an installed copy, not a zip one
    bool caseInsensitivePaths = false;  ///< Paths compare ignoring case (Windows)
};

class IStartAtLogin
{
  public:
    IStartAtLogin() = default;
    virtual ~IStartAtLogin() = default;

    IStartAtLogin(const IStartAtLogin&) = delete;
    IStartAtLogin& operator=(const IStartAtLogin&) = delete;
    IStartAtLogin(IStartAtLogin&&) = delete;
    IStartAtLogin& operator=(IStartAtLogin&&) = delete;

    /// The entry, and this copy's path. A value that isn't text reads as an empty command.
    /// @return why it can't be read, for Settings' line.
    [[nodiscard]] virtual std::expected<StartAtLoginFacts, std::string> read() = 0;

    /// Writes the entry as @p command (Domain::StartAtLogin::runCommand()), replacing one that is there.
    [[nodiscard]] virtual std::expected<void, std::string> write(std::string_view command) = 0;

    /// Removes the entry; success when there is none. Nothing else is touched.
    [[nodiscard]] virtual std::expected<void, std::string> remove() = 0;

    /// Whether a missing entry the user had turned on is written back after an install (#3276): on Windows,
    /// where an installer's uninstall step can remove the Run value and Windows' own startup settings never
    /// delete it (they disable it elsewhere). Not on Linux, where a desktop's startup settings delete the file
    /// to turn it off. Answers from what the class is: no read, no I/O.
    [[nodiscard]] virtual bool restoresMissingEntry() const noexcept
    {
        return false;
    }

    /// Whether TaskSmack was uninstalled since it last ran (#3276), and clears the mark that says so. A real
    /// uninstall (one no installer ran as an upgrade) leaves it beside the entry, on Windows the HKCU value
    /// Software\TaskSmack Uninstalled (cmake/CPackProjectConfig.cmake.in); then what the user had turned on is
    /// forgotten instead of being written back. Blocking (registry calls): on the setting's worker. False
    /// where there is no uninstaller that leaves one (Linux).
    [[nodiscard]] virtual std::expected<bool, std::string> takeUninstallMark()
    {
        return false;
    }
};

} // namespace Platform
