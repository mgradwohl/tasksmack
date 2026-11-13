#pragma once

// Restart as administrator (#2728): on Windows, Administrator is TaskSmack restarted elevated, so nothing is
// limited in that session. TaskSmack always starts as Standard; only the user's choice (the startup notice, or
// the status bar's Standard chip, which opens it) restarts it.
//
// The hand-off: the Standard TaskSmack starts its own executable with the "runas" verb, which raises the UAC
// prompt, passing HANDOFF_ARGUMENT with its PID and its creation time, then CONFIG_DIR_ARGUMENT and the launching
// user's config directory. Once Windows returns the elevated process (the prompt was approved) the Standard one
// closes, saving its settings and window as any close does. The elevated one (main.cpp):
//   - waits for that process to exit, when it may open it and its creation time matches (a reused PID is not
//     waited for); another account's elevation often may not open it at all;
//   - so the single-instance lock is the hand-off itself: the elevated copy tries it again for up to
//     HANDOFF_LOCK_PATIENCE before it gives up, and reads config.toml only once it holds it, after the Standard
//     copy's last save;
//   - takes the handed-over directory only once it passes Platform::checkHandedOverConfigDir().
//
// The trust boundary (#2728 decision 4): with an over-the-shoulder prompt, or Administrator Protection, the
// elevated copy runs as another account than the user who chose the directory, and anything running as that
// user could have started this restart, or planted links in that directory. So the directory must be a real
// folder under the user profiles, with no junction or link on its path, owned by no administrator or system
// account, its config.toml and lock file neither links nor hard-linked; it is held open so it can't be swapped;
// UserConfig never writes through a link in it (ConfigDirOverride::handedOver()); its recent Run new task
// commands aren't offered for an elevated run, and its dump folder isn't written to (DumpDetail::trustedDumpFolder()). A directory that
// fails is not used: the elevated copy keeps its own account's settings, and says so in the log.
//
// Null on Linux, where an Administrator session is started as one (#3580), and in an elevated TaskSmack.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
namespace Platform
{

/// The elevated copy's first argument, followed by the Standard TaskSmack's PID and its creation time (a FILETIME's
/// ticks, as BrokerHelper::startTicksOf() reads it), which it waits for.
inline constexpr std::string_view HANDOFF_ARGUMENT = "--handoff";
/// Followed by the launching user's config directory, which the elevated copy uses for everything kept there.
inline constexpr std::string_view CONFIG_DIR_ARGUMENT = "--config-dir";

/// How long an elevated restart keeps trying the single-instance lock the Standard copy holds while it closes.
inline constexpr std::chrono::milliseconds HANDOFF_LOCK_PATIENCE{30'000};

/// How the elevated copy's wait for the Standard one ended.
enum class HandoffWait : std::uint8_t
{
    Exited,         ///< It closed
    NotRunning,     ///< No such process: it had already gone
    NotThatProcess, ///< The PID is another process's now (its creation time differs): not waited for
    Unopenable,     ///< It may not be opened (another account's elevation): the lock is the hand-off
    TimedOut,       ///< It still runs after the wait
};

/// What Windows' prompt will ask for.
enum class AdministratorPrompt : std::uint8_t
{
    Consent,     ///< An administrator's account: approval only
    Credentials, ///< A standard user's: an administrator's name and password
};

/// How a restart went.
struct ElevatedRestartResult
{
    enum class Outcome : std::uint8_t
    {
        Started,  ///< The elevated copy runs: close this one
        Declined, ///< The prompt was dismissed: carry on as Standard
        Failed,   ///< Something else stopped it (detail says what): carry on as Standard
    };

    Outcome outcome = Outcome::Failed;
    std::string detail; ///< Failed: why, in a sentence fragment
};

class IElevatedRestart
{
  public:
    IElevatedRestart() = default;
    virtual ~IElevatedRestart() = default;

    IElevatedRestart(const IElevatedRestart&) = delete;
    IElevatedRestart& operator=(const IElevatedRestart&) = delete;
    IElevatedRestart(IElevatedRestart&&) = delete;
    IElevatedRestart& operator=(IElevatedRestart&&) = delete;

    /// What the prompt will ask for, read once when this was made.
    [[nodiscard]] virtual AdministratorPrompt prompt() const noexcept = 0;

    /// Starts TaskSmack elevated with @p configDir as its config directory. Blocks while the UAC prompt is
    /// up, with no time limit, so it is called on a worker, never the UI thread.
    [[nodiscard]] virtual ElevatedRestartResult restart(const std::filesystem::path& configDir) = 0;
};

} // namespace Platform
