#pragma once

// The sign-in entry that starts TaskSmack when the user signs in (#3093, part 2): raw reads and writes,
// no decisions (those are Domain/StartAtLogin.h's). Windows: the per-user Run key,
// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "TaskSmack" (WindowsStartAtLogin); no elevation.
// Linux: an XDG autostart entry, agent:WSL's part (#3254), not written yet, so Platform::makeStartAtLogin()
// gives null there and Settings hides the row.
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
};

} // namespace Platform
