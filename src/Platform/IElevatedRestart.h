#pragma once

// Restart as administrator (#2728): on Windows, Administrator is TaskSmack restarted elevated, so nothing is
// limited in that session. TaskSmack always starts as Standard; only the user's choice (the startup notice, or
// the status bar's Standard chip, which opens it) restarts it.
//
// The hand-off: the Standard TaskSmack starts its own executable with the "runas" verb, which raises the UAC
// prompt, passing HANDOFF_ARGUMENT and its PID, then CONFIG_DIR_ARGUMENT and the launching user's config
// directory. Once Windows returns the elevated process (the prompt was approved) the Standard one closes,
// saving its settings and window as any close does. The elevated one waits for it to exit before it reads
// those settings or takes the single-instance lock (main.cpp), and keeps the launching user's settings even
// when another account approved the prompt (an over-the-shoulder prompt, or Administrator Protection).
//
// Null on Linux, where an Administrator session is started as one (#3580), and in an elevated TaskSmack.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Platform
{

/// The elevated copy's first argument, followed by the Standard TaskSmack's PID, which it waits for.
inline constexpr std::string_view HANDOFF_ARGUMENT = "--handoff";
/// Followed by the launching user's config directory, which the elevated copy uses for everything kept there.
inline constexpr std::string_view CONFIG_DIR_ARGUMENT = "--config-dir";

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
