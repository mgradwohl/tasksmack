#pragma once

#include "Platform/IStartupProbe.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform
{

/// Windows startup entries (#801), read-only and without elevation.
///
/// Reads the Run and RunOnce keys (HKCU, HKLM and HKLM's 32-bit view), the user and all-users
/// Startup folders (shortcut targets through IShellLinkW, with COM initialised for the duration of
/// each enumerate() on the calling thread), the enabled state and disable time from
/// Explorer\StartupApproved, and each executable's version-resource CompanyName (cached per file
/// until its last-write time changes).
class WindowsStartupProbe : public IStartupProbe
{
  public:
    WindowsStartupProbe();
    ~WindowsStartupProbe() override;

    WindowsStartupProbe(const WindowsStartupProbe&) = delete;
    WindowsStartupProbe& operator=(const WindowsStartupProbe&) = delete;
    WindowsStartupProbe(WindowsStartupProbe&&) = delete;
    WindowsStartupProbe& operator=(WindowsStartupProbe&&) = delete;

    [[nodiscard]] StartupCapabilities capabilities() const override;
    [[nodiscard]] std::vector<StartupEntry> enumerate() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
};

namespace Windows
{

/// `text` with %VARIABLE% references expanded by ExpandEnvironmentStringsW; unknown variables are
/// left as written. UTF-8 in and out.
[[nodiscard]] std::string expandEnvironmentStrings(std::string_view text);

/// The executable `commandLine` starts, as WindowsStartupProbe resolves it: environment variables
/// expanded, then StartupMath::executableFromCommandLine, then a bare file name ("ctfmon.exe")
/// searched for on the search path. Empty when there is none.
[[nodiscard]] std::string resolveStartupExecutable(std::string_view commandLine);

/// A shortcut (.lnk) file's target and arguments as one command line ("target" args), environment
/// variables as stored; nullopt when it can't be loaded or has no file-system target (e.g. an
/// advertised installer shortcut). COM must be initialised on the calling thread.
[[nodiscard]] std::optional<std::string> readShortcutCommand(const std::filesystem::path& shortcut);

} // namespace Windows

} // namespace Platform
