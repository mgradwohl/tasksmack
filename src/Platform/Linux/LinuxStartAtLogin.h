#pragma once

// Start at sign-in on Linux (#3254, #3093 part 2): a per-user XDG autostart entry,
// $XDG_CONFIG_HOME/autostart/tasksmack.desktop (~/.config/autostart/tasksmack.desktop), as the Settings
// checkbox turns it on and off. Per user, so no root; only that one file is ever read, written or removed,
// and the desktop shows it in its own startup settings. The Startup tab (#2541) lists it like any other.
//
// The entry's command travels as Domain::StartAtLogin's command line ("<quoted path>" --minimized). In the
// file it is an Exec value, escaped as the Desktop Entry Specification asks: the program quoted, with the
// quoting rule's backslash escapes (\ " ` $), % doubled, then the string type's own backslash escape
// (tools/install-local.sh does the same, #2342). read() undoes both and hands the command line back.
//
// Blocking (file calls): used on a worker (App::StartAtLoginSetting), never on the UI thread (#2731).

#include "Platform/IStartAtLogin.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace Platform
{

namespace StartAtLoginEntry
{

/// The entry's file name in the autostart directory.
inline constexpr std::string_view FILE_NAME = "tasksmack.desktop";

/// The Exec value for @p command (Domain::StartAtLogin::runCommand()'s "<quoted path> <arguments>"): the
/// program quoted and escaped as the Desktop Entry Specification asks, then the arguments, each word as
/// it is. nullopt for a command whose program can't be found, or that holds a newline (an Exec can't).
[[nodiscard]] std::optional<std::string> execValue(std::string_view command);

/// The command line in an Exec value, as Domain::StartAtLogin reads one: the program quoted, then the rest.
/// The string type's escapes and the quoting rule's are undone, and %% is %. nullopt for an empty Exec, or a
/// quote that never closes.
[[nodiscard]] std::optional<std::string> commandFromExec(std::string_view exec);

/// The whole entry file for @p exec.
[[nodiscard]] std::string entryText(std::string_view exec);

/// The autostart directory: $XDG_CONFIG_HOME/autostart when that is an absolute path, else
/// $HOME/.config/autostart; empty when neither is known.
[[nodiscard]] std::filesystem::path autostartDirectory(const char* xdgConfigHome, const char* home);

} // namespace StartAtLoginEntry

class LinuxStartAtLogin final : public IStartAtLogin
{
  public:
    /// The real one: the autostart directory from the environment, the running binary from /proc/self/exe.
    LinuxStartAtLogin();
    /// Tests: an autostart directory and an executable path of their own.
    LinuxStartAtLogin(std::filesystem::path autostartDirectory, std::string executable);

    [[nodiscard]] std::expected<StartAtLoginFacts, std::string> read() override;
    [[nodiscard]] std::expected<void, std::string> write(std::string_view command) override;
    [[nodiscard]] std::expected<void, std::string> remove() override;

  private:
    [[nodiscard]] std::filesystem::path entryPath() const;

    std::filesystem::path m_Directory; ///< Empty when no home is known: every call then says so
    std::string m_Executable;
};

} // namespace Platform
